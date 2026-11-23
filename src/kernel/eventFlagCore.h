#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_EVENTFLAGCORE_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_EVENTFLAGCORE_H_

// The event flag object without the libkernel entry points (kernel/eventFlag.cpp), so a test exe can
// link it with `common` only.
//
// Two ways to release a waiter (fixed per flag at creation):
// - AtWake (KYTY_EVFLAG_RELEASE_AT_SET=0, the default): Set wakes every waiter, and each one re-tests
//   its pattern when it runs. A waiter whose pattern held at the Set but not when it runs (another
//   waiter's or a later Wait's clear took the bits first) sleeps on: a "would-miss", counted.
// - AtSet (=1, upstream KytyPS5 727ccd1c, release part only): Set tests the waiters in queue order
//   and releases each one whose pattern holds, with its result and its clear applied there, before
//   the next one is tested. A released waiter always returns Ok, even when its timeout ran out or a
//   Cancel/Delete came before it ran (its clear is already applied). A release whose pattern no
//   longer holds when the waiter runs is counted as "saved" (the same case seen from this side).

#include "common/common.h"
#include "common/threads.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Libs::LibKernel::EventFlag {

// Totals over all flags (relaxed atomics), for the NHL27EVFLAG line.
struct EventFlagCounters {
	std::atomic<uint64_t> flags {0};             // live flags
	std::atomic<uint64_t> waits {0};             // waits that had to sleep
	std::atomic<uint64_t> sets {0};
	std::atomic<uint64_t> multi_waiter_sets {0}; // Sets with 2+ queued waiters
	std::atomic<uint64_t> would_miss {0};        // AtWake only
	std::atomic<uint64_t> saved {0};             // AtSet only
	std::atomic<uint64_t> longest_wait_us {0};   // the reader takes and resets it
};

EventFlagCounters& Counters();

// One would-miss or saved release, for the NHL27EVMISS line. Reported after the flag's lock is released.
struct MissEvent {
	char     name[32];
	uint64_t pattern;
	uint64_t bits_at_set;
	uint64_t bits_now;
	uint32_t waiters; // queued when the waiter ran
	bool     and_mode;
	uint8_t  clear;   // 0 none, 1 all, 2 bits
	bool     saved;   // false = would-miss
};

using MissHook = void (*)(const MissEvent& event);
void SetMissHook(MissHook hook);

// Freeze report: one line naming each live flag with waiters (name, bits, each waiter's pattern, mode
// and age). Never blocks: TryLock only, "busy" when a lock is held. Returns the length written.
size_t DescribeWaitingFlags(char* text, size_t size);

class KernelEventFlagPrivate {
public:
	enum class Result { Ok, AlreadyWaiting, TimedOut, Canceled, Deleted };

	enum class ClearMode { None, All, Bits };

	enum class WaitMode { And, Or };

	enum class Release { AtWake, AtSet };

	KernelEventFlagPrivate(const std::string& name, bool single, bool fifo, uint64_t bits,
	                       Release release = Release::AtWake);
	virtual ~KernelEventFlagPrivate();

	KYTY_CLASS_NO_COPY(KernelEventFlagPrivate);

	void   Set(uint64_t bits);
	void   Clear(uint64_t bits);
	void   Cancel(uint64_t bits, int* num_waiting_threads);
	// priority: the caller's thread priority; only used when the flag is not FIFO.
	Result Wait(uint64_t bits, WaitMode wait_mode, ClearMode clear_mode, uint64_t* result,
	            uint32_t* ptr_micros, int priority = 0);

	Result Poll(uint64_t bits, WaitMode wait_mode, ClearMode clear_mode, uint64_t* result) {
		uint32_t micros = 0;
		return Wait(bits, wait_mode, clear_mode, result, &micros);
	}

	[[nodiscard]] bool Fifo() const { return m_fifo; }

	// The AtWake would-miss test, out of line so the tests can check it without a thread race: the
	// pattern held at a Set while the waiter slept, and does not hold now.
	[[nodiscard]] static bool WouldMiss(bool held_at_set, uint64_t bits_now, uint64_t pattern, WaitMode wait_mode);
	[[nodiscard]] static bool Satisfied(uint64_t bits_now, uint64_t pattern, WaitMode wait_mode) {
		return (wait_mode == WaitMode::And ? (bits_now & pattern) == pattern : (bits_now & pattern) != 0);
	}

#ifdef KYTY_EVENT_FLAG_TESTS
	// Test hooks (event_flag_tests only): each runs its steps under one hold of the flag's lock, so the
	// woken waiters see them all at once.
	size_t QueuedForTest();
	void   HoldThenSetForTest(uint64_t bits, uint32_t hold_micros); // the waiter's timeout runs out first
	void   SetThenCancelForTest(uint64_t set_bits, uint64_t cancel_bits);
	void   SetThenStealForTest(uint64_t bits, uint64_t steal_pattern); // a clear-mode Wait takes the bits first
#endif

private:
	friend size_t DescribeWaitingFlags(char* text, size_t size);

	enum class Status { Set, Canceled, Deleted };

	// A waiting thread, on its stack, queued while it sleeps.
	struct Waiter {
		uint64_t  pattern     = 0;
		WaitMode  wait_mode   = WaitMode::And;
		ClearMode clear_mode  = ClearMode::None;
		int       priority    = 0;
		uint64_t  start_ns    = 0;
		uint64_t  result      = 0; // AtSet: the bits at its release
		uint64_t  bits_at_set = 0; // the bits after the Set that satisfied it
		bool      held_at_set = false;
		bool      released    = false;
	};

	Result WaitAtWake(uint64_t bits, WaitMode wait_mode, ClearMode clear_mode, uint64_t* result,
	                  uint32_t* ptr_micros, int priority, MissEvent* event, bool* have_event);
	Result WaitAtSet(uint64_t bits, WaitMode wait_mode, ClearMode clear_mode, uint64_t* result,
	                 uint32_t* ptr_micros, int priority, MissEvent* event, bool* have_event);
	void   WaitForSetStatus(); // with m_mutex held
	void   SetLocked(uint64_t bits);
	void   CancelLocked(uint64_t bits, int* num_waiting_threads);
	void   Enqueue(Waiter* waiter);
	void   Dequeue(Waiter* waiter);
	void   ApplyClear(uint64_t pattern, ClearMode clear_mode);
	void   FillEvent(const Waiter& waiter, bool saved, MissEvent* event) const;
	void   Describe(char* text, size_t size, size_t* used, uint64_t now_ns) const;

	Common::Mutex        m_mutex;
	Common::CondVar      m_cond_var;
	Status               m_status          = Status::Set;
	int                  m_waiting_threads = 0;
	std::string          m_name;
	bool                 m_single_thread = false;
	bool                 m_fifo          = true;
	Release              m_release       = Release::AtWake;
	uint64_t             m_bits          = 0;
	std::vector<Waiter*> m_queue;
};

} // namespace Libs::LibKernel::EventFlag

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_EVENTFLAGCORE_H_ */
