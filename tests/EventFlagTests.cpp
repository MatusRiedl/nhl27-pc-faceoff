// Unit tests for the event flag core (kernel/eventFlagCore.cpp), both release modes. Built with
// KYTY_EVENT_FLAG_TESTS, which adds the test hooks that run several steps under one hold of the flag's
// lock, so the thread races are fixed and the tests are not flaky.

#include "kernel/eventFlagCore.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {

using Libs::LibKernel::EventFlag::Counters;
using Libs::LibKernel::EventFlag::DescribeWaitingFlags;
using Libs::LibKernel::EventFlag::MissEvent;
using Libs::LibKernel::EventFlag::SetMissHook;
using Flag = Libs::LibKernel::EventFlag::KernelEventFlagPrivate;
using Result = Flag::Result;
using ClearMode = Flag::ClearMode;
using WaitMode = Flag::WaitMode;
using Release = Flag::Release;

const char *g_mode_name = "";

void Check(bool value, const char *text) {
  if (!value) {
    std::fprintf(stderr, "EventFlagTests: failed (%s): %s\n", g_mode_name, text);
    std::abort();
  }
}

void SleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// Waits until `count` threads are queued (and so asleep: a waiter queues and sleeps under one lock hold).
void WaitQueued(Flag *flag, size_t count) {
  for (int i = 0; i < 5000; i++) {
    if (flag->QueuedForTest() == count) {
      return;
    }
    SleepMs(1);
  }
  Check(false, "waiters did not queue in 5 s");
}

struct WaitOutcome {
  Result result = Result::Ok;
  uint64_t bits = 0;
  uint32_t micros = 0;
};

// Starts a waiter thread. micros == UINT32_MAX means wait without a timeout.
std::thread StartWaiter(Flag *flag, uint64_t pattern, WaitMode wait_mode, ClearMode clear_mode,
                        uint32_t micros, WaitOutcome *out, int priority = 0) {
  return std::thread([=]() {
    out->micros = micros;
    uint32_t *ptr = (micros == UINT32_MAX ? nullptr : &out->micros);
    out->result = flag->Wait(pattern, wait_mode, clear_mode, &out->bits, ptr, priority);
  });
}

uint64_t Bits(Flag *flag) {
  uint64_t bits = 0;
  (void)flag->Poll(0, WaitMode::Or, ClearMode::None, &bits); // pattern 0 never holds in Or mode
  return bits;
}

MissEvent g_last_event{};
std::atomic<int> g_events{0};

void OnMiss(const MissEvent &event) {
  g_last_event = event;
  g_events.fetch_add(1);
}

// --- Both modes ----------------------------------------------------------------------------------

void TestSetReleasesWaiter(Release release) {
  Flag flag("t_release", false, true, 0x10, release);

  WaitOutcome a;
  auto t = StartWaiter(&flag, 0x4, WaitMode::And, ClearMode::None, UINT32_MAX, &a);
  WaitQueued(&flag, 1);
  flag.Set(0x4);
  t.join();
  Check(a.result == Result::Ok, "Set releases a waiter without a timeout");
  Check(a.bits == 0x14, "the waiter gets the bits after the Set");
  Check(flag.QueuedForTest() == 0, "the released waiter leaves the queue");

  WaitOutcome b;
  t = StartWaiter(&flag, 0x1, WaitMode::Or, ClearMode::All, 2000000, &b);
  WaitQueued(&flag, 1);
  flag.Set(0x1);
  t.join();
  Check(b.result == Result::Ok, "Set releases a timed waiter");
  Check(b.bits == 0x15, "the timed waiter gets the bits");
  Check(b.micros > 0 && b.micros <= 2000000, "the timed waiter gets the time left");
  Check(Bits(&flag) == 0, "clear mode All clears every bit");
}

void TestTimeoutAndPoll(Release release) {
  Flag flag("t_timeout", false, true, 0x2, release);

  uint64_t bits = 0;
  uint32_t micros = 20000;
  Check(flag.Wait(0x1, WaitMode::And, ClearMode::None, &bits, &micros) == Result::TimedOut,
        "a wait for a missing bit times out");
  Check(bits == 0x2, "a timeout returns the current bits");
  Check(micros == 0, "a timeout returns no time left");

  bits = 0;
  Check(flag.Poll(0x1, WaitMode::And, ClearMode::None, &bits) == Result::TimedOut,
        "a poll for a missing bit fails at once");
  Check(bits == 0x2, "a failed poll returns the current bits");

  flag.Set(0x5);
  Check(flag.Poll(0x5, WaitMode::And, ClearMode::Bits, &bits) == Result::Ok, "a poll for set bits passes");
  Check(bits == 0x7, "a poll returns the bits before its clear");
  Check(Bits(&flag) == 0x2, "clear mode Bits clears only the pattern");

  flag.Clear(0x0);
  Check(Bits(&flag) == 0, "Clear keeps only the bits in its mask");
}

void TestCancel(Release release) {
  Flag flag("t_cancel", false, true, 0, release);

  WaitOutcome a;
  auto t = StartWaiter(&flag, 0x1, WaitMode::And, ClearMode::None, UINT32_MAX, &a);
  WaitQueued(&flag, 1);
  int waiting = -1;
  flag.Cancel(0x8, &waiting);
  t.join();
  Check(waiting == 1, "Cancel counts the waiting threads");
  Check(a.result == Result::Canceled, "Cancel releases a waiter with Canceled");
  Check(a.bits == 0x8, "a canceled waiter gets the bits Cancel set");
  Check(flag.QueuedForTest() == 0, "Cancel empties the queue");
  Check(Bits(&flag) == 0x8, "Cancel sets the bits");

  // The flag works again after a Cancel.
  WaitOutcome b;
  t = StartWaiter(&flag, 0x1, WaitMode::And, ClearMode::None, UINT32_MAX, &b);
  WaitQueued(&flag, 1);
  flag.Set(0x1);
  t.join();
  Check(b.result == Result::Ok, "a wait after a Cancel is released by Set");
}

void TestDelete(Release release) {
  auto *flag = new Flag("t_delete", false, true, 0x2, release);

  WaitOutcome a;
  auto t = StartWaiter(flag, 0x1, WaitMode::And, ClearMode::None, UINT32_MAX, &a);
  WaitQueued(flag, 1);
  delete flag; // waits until the waiter has left
  t.join();
  Check(a.result == Result::Deleted, "Delete releases a waiter with Deleted");
  Check(a.bits == 0x2, "a deleted waiter gets the current bits");
}

void TestSingleWaiter(Release release) {
  Flag flag("t_single", true, true, 0, release);

  WaitOutcome a;
  auto t = StartWaiter(&flag, 0x1, WaitMode::And, ClearMode::None, UINT32_MAX, &a);
  WaitQueued(&flag, 1);
  uint64_t bits = 0;
  uint32_t micros = 1000;
  Check(flag.Wait(0x2, WaitMode::And, ClearMode::None, &bits, &micros) == Result::AlreadyWaiting,
        "a second waiter on a single-waiter flag gets AlreadyWaiting");
  flag.Set(0x1);
  t.join();
  Check(a.result == Result::Ok, "the first waiter is still released");
}

// The Set comes after the waiter's timeout ran out but before the waiter runs: it returns Ok.
void TestReleaseWinsOverTimeout(Release release) {
  Flag flag("t_late_set", false, true, 0, release);

  WaitOutcome a;
  auto t = StartWaiter(&flag, 0x1, WaitMode::And, ClearMode::Bits, 50000, &a);
  WaitQueued(&flag, 1);
  flag.HoldThenSetForTest(0x1, 150000);
  t.join();
  Check(a.result == Result::Ok, "a Set before the waiter runs wins over its timeout");
  Check(a.bits == 0x1, "the late-released waiter gets the Set's bits");
  Check(a.micros == 0, "the late-released waiter has no time left");
  Check(Bits(&flag) == 0, "the late-released waiter's clear is applied");
}

void TestDescribe(Release release) {
  Flag flag("t_describe", false, true, 0x2, release);

  char text[512];
  DescribeWaitingFlags(text, sizeof(text));
  Check(std::strstr(text, "t_describe") == nullptr, "a flag with no waiters is not listed");

  WaitOutcome a;
  auto t = StartWaiter(&flag, 0x1, WaitMode::Or, ClearMode::All, UINT32_MAX, &a);
  WaitQueued(&flag, 1);
  DescribeWaitingFlags(text, sizeof(text));
  Check(std::strstr(text, "t_describe bits=0x2 [0x1 or clr=1 ") != nullptr, "a waiting flag is listed");

  // A held flag lock gives "busy" and never blocks the report.
  std::atomic<bool> done{false};
  std::thread holder([&]() {
    flag.HoldThenSetForTest(0x1, 300000);
    done = true;
  });
  bool busy = false;
  while (!done && !busy) {
    DescribeWaitingFlags(text, sizeof(text));
    busy = (std::strstr(text, "t_describe:busy") != nullptr);
    SleepMs(1);
  }
  holder.join();
  t.join();
  Check(busy, "a held flag lock is reported as busy");
  Check(a.result == Result::Ok, "the waiter is released after the hold");

  char small[8];
  DescribeWaitingFlags(small, sizeof(small));
  Check(std::strlen(small) < sizeof(small), "a short buffer is not overrun");
}

// --- Release at Set (mode 1) ---------------------------------------------------------------------

// Upstream's queue-order case: A (no clear) and B (clear) both wait for bit 1; one Set releases both
// in queue order, and the bit ends up cleared.
void TestAtSetQueueOrder() {
  Flag flag("t_order", false, true, 0, Release::AtSet);
  const uint64_t sets0 = Counters().sets.load();
  const uint64_t multi0 = Counters().multi_waiter_sets.load();
  const uint64_t waits0 = Counters().waits.load();

  WaitOutcome a;
  WaitOutcome b;
  auto ta = StartWaiter(&flag, 0x1, WaitMode::And, ClearMode::None, UINT32_MAX, &a);
  WaitQueued(&flag, 1);
  auto tb = StartWaiter(&flag, 0x1, WaitMode::And, ClearMode::Bits, UINT32_MAX, &b);
  WaitQueued(&flag, 2);
  flag.Set(0x1);
  ta.join();
  tb.join();
  Check(a.result == Result::Ok && b.result == Result::Ok, "one Set releases both waiters");
  Check(a.bits == 0x1 && b.bits == 0x1, "both waiters get the Set's bits");
  Check(Bits(&flag) == 0, "the second waiter's clear is applied");
  Check(Counters().sets.load() - sets0 == 1, "the Set is counted");
  Check(Counters().multi_waiter_sets.load() - multi0 == 1, "a Set with 2 waiters is counted");
  Check(Counters().waits.load() - waits0 == 2, "both sleeping waits are counted");

  // Reverse order: B's clear takes the bit, so A stays queued (the bit was used once).
  ta = StartWaiter(&flag, 0x1, WaitMode::And, ClearMode::Bits, UINT32_MAX, &b);
  WaitQueued(&flag, 1);
  tb = StartWaiter(&flag, 0x1, WaitMode::And, ClearMode::None, 50000, &a);
  WaitQueued(&flag, 2);
  flag.Set(0x1);
  ta.join();
  tb.join();
  Check(b.result == Result::Ok, "the first (clear) waiter is released");
  Check(a.result == Result::TimedOut, "the second waiter is not released after the first one's clear");
  Check(a.bits == 0, "the second waiter sees the bit cleared");
}

// A non-FIFO flag releases by priority (a lower value first; equal values in arrival order).
void TestAtSetPriorityOrder() {
  Flag flag("t_prio", false, false, 0, Release::AtSet);

  WaitOutcome low;
  WaitOutcome high;
  auto tl = StartWaiter(&flag, 0x1, WaitMode::And, ClearMode::Bits, UINT32_MAX, &low, 700);
  WaitQueued(&flag, 1);
  auto th = StartWaiter(&flag, 0x1, WaitMode::And, ClearMode::Bits, UINT32_MAX, &high, 100);
  WaitQueued(&flag, 2);
  flag.Set(0x1);
  th.join();
  Check(high.result == Result::Ok, "the higher-priority waiter is released first");
  Check(flag.QueuedForTest() == 1, "the lower-priority waiter stays queued after the clear");
  flag.Set(0x1);
  tl.join();
  Check(low.result == Result::Ok, "the next Set releases the lower-priority waiter");
}

// Released, then Canceled before it runs: the release wins (its clear is already applied).
void TestAtSetReleaseWinsOverCancel() {
  Flag flag("t_set_cancel", false, true, 0x10, Release::AtSet);

  WaitOutcome a;
  auto t = StartWaiter(&flag, 0x1, WaitMode::And, ClearMode::None, UINT32_MAX, &a);
  WaitQueued(&flag, 1);
  flag.SetThenCancelForTest(0x1, 0x8);
  t.join();
  Check(a.result == Result::Ok, "a release wins over a later Cancel");
  Check(a.bits == 0x11, "the released waiter gets the Set's bits, not the Cancel's");
}

// The one-waiter case: a later clear-mode Wait takes the bits before the waiter runs.
void TestAtSetSaved() {
  Flag flag("t_saved", false, true, 0, Release::AtSet);
  const uint64_t saved0 = Counters().saved.load();
  const uint64_t miss0 = Counters().would_miss.load();
  const int events0 = g_events.load();

  WaitOutcome a;
  auto t = StartWaiter(&flag, 0x1, WaitMode::And, ClearMode::None, UINT32_MAX, &a);
  WaitQueued(&flag, 1);
  flag.SetThenStealForTest(0x1, 0x1);
  t.join();
  Check(a.result == Result::Ok, "the waiter is released although the bits were taken");
  Check(a.bits == 0x1, "the waiter gets the Set's bits");
  Check(Counters().saved.load() - saved0 == 1, "the saved release is counted");
  Check(Counters().would_miss.load() == miss0, "no would-miss in mode 1");
  Check(g_events.load() - events0 == 1, "the saved release is reported");
  Check(g_last_event.saved && std::strcmp(g_last_event.name, "t_saved") == 0, "the report names the flag");
  Check(g_last_event.pattern == 0x1 && g_last_event.bits_at_set == 0x1 && g_last_event.bits_now == 0,
        "the report has the pattern and the bits");
}

// --- Release at wake (mode 0, today's behaviour) -------------------------------------------------

void TestAtWakeCancelAfterSet() {
  Flag flag("t_wake_cancel", false, true, 0x10, Release::AtWake);

  WaitOutcome a;
  auto t = StartWaiter(&flag, 0x1, WaitMode::And, ClearMode::None, UINT32_MAX, &a);
  WaitQueued(&flag, 1);
  flag.SetThenCancelForTest(0x1, 0x8);
  t.join();
  Check(a.result == Result::Canceled, "mode 0 keeps today's result: Canceled");
  Check(a.bits == 0x8, "mode 0 returns the Cancel's bits");
}

// The same one-waiter case: mode 0 sleeps on and counts a would-miss.
void TestAtWakeWouldMiss() {
  Flag flag("t_miss", false, true, 0, Release::AtWake);
  const uint64_t miss0 = Counters().would_miss.load();
  const uint64_t saved0 = Counters().saved.load();
  const int events0 = g_events.load();

  WaitOutcome a;
  auto t = StartWaiter(&flag, 0x1, WaitMode::And, ClearMode::None, 300000, &a);
  WaitQueued(&flag, 1);
  flag.SetThenStealForTest(0x1, 0x1);
  t.join();
  Check(a.result == Result::TimedOut, "mode 0 misses the wake-up and times out");
  Check(Counters().would_miss.load() - miss0 == 1, "the would-miss is counted once");
  Check(Counters().saved.load() == saved0, "no saved release in mode 0");
  Check(g_events.load() - events0 == 1, "the would-miss is reported");
  Check(!g_last_event.saved && std::strcmp(g_last_event.name, "t_miss") == 0, "the report names the flag");
  Check(g_last_event.pattern == 0x1 && g_last_event.bits_at_set == 0x1 && g_last_event.bits_now == 0 &&
            g_last_event.and_mode && g_last_event.clear == 0,
        "the report has the pattern, the mode and the bits");
}

void TestWouldMissRule() {
  Check(Flag::WouldMiss(true, 0x0, 0x1, WaitMode::And), "held at the Set, gone now: a miss");
  Check(!Flag::WouldMiss(false, 0x0, 0x1, WaitMode::And), "never held: no miss");
  Check(!Flag::WouldMiss(true, 0x1, 0x1, WaitMode::And), "still held: no miss");
  Check(Flag::WouldMiss(true, 0x1, 0x3, WaitMode::And), "and mode needs every bit");
  Check(!Flag::WouldMiss(true, 0x2, 0x3, WaitMode::Or), "or mode needs one bit");
  Check(Flag::WouldMiss(true, 0x4, 0x3, WaitMode::Or), "or mode with no pattern bit: a miss");
}

void RunBothModes(Release release, const char *name) {
  g_mode_name = name;
  TestSetReleasesWaiter(release);
  TestTimeoutAndPoll(release);
  TestCancel(release);
  TestDelete(release);
  TestSingleWaiter(release);
  TestReleaseWinsOverTimeout(release);
  TestDescribe(release);
}

} // namespace

int main() {
  SetMissHook(OnMiss);

  RunBothModes(Release::AtWake, "release at wake");
  RunBothModes(Release::AtSet, "release at set");

  g_mode_name = "release at set";
  TestAtSetQueueOrder();
  TestAtSetPriorityOrder();
  TestAtSetReleaseWinsOverCancel();
  TestAtSetSaved();

  g_mode_name = "release at wake";
  TestAtWakeCancelAfterSet();
  TestAtWakeWouldMiss();

  g_mode_name = "rule";
  TestWouldMissRule();

  Check(Counters().flags.load() == 0, "every flag left the registry");
  std::printf("EventFlagTests: all passed\n");
  return 0;
}
