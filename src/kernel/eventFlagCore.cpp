#include "kernel/eventFlagCore.h"

#include "common/assert.h"
#include "common/timer.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace Libs::LibKernel::EventFlag {

namespace {

// The live flags, for the freeze report. Leaked on purpose: game threads may still delete flags while
// the process exits.
struct Registry {
	Common::Mutex                        mutex;
	std::vector<KernelEventFlagPrivate*> flags;
};

Registry& GetRegistry() {
	static auto* registry = new Registry;
	return *registry;
}

std::atomic<MissHook> g_miss_hook {nullptr};

uint64_t NowNs() {
	return static_cast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
	        .count());
}

void NoteWaitTime(const Common::Timer& t) {
	const auto us   = static_cast<uint64_t>(t.GetTimeS() * 1000000.0);
	auto&      slot = Counters().longest_wait_us;
	uint64_t   old  = slot.load(std::memory_order_relaxed);
	while (us > old && !slot.compare_exchange_weak(old, us, std::memory_order_relaxed)) {
	}
}

void Append(char* text, size_t size, size_t* used, const char* format, ...) {
	if (*used + 1 >= size) {
		return;
	}
	va_list args;
	va_start(args, format);
	const int n = std::vsnprintf(text + *used, size - *used, format, args);
	va_end(args);
	if (n > 0) {
		*used = std::min(size - 1, *used + static_cast<size_t>(n));
	}
}

} // namespace

EventFlagCounters& Counters() {
	static EventFlagCounters counters;
	return counters;
}

void SetMissHook(MissHook hook) {
	g_miss_hook.store(hook, std::memory_order_release);
}

KernelEventFlagPrivate::KernelEventFlagPrivate(const std::string& name, bool single, bool fifo, uint64_t bits,
                                               Release release)
    : m_name(name), m_single_thread(single), m_fifo(fifo), m_release(release), m_bits(bits) {
	auto&             registry = GetRegistry();
	Common::LockGuard lock(registry.mutex);
	registry.flags.push_back(this);
	Counters().flags.fetch_add(1, std::memory_order_relaxed);
}

KernelEventFlagPrivate::~KernelEventFlagPrivate() {
	{
		auto&             registry = GetRegistry();
		Common::LockGuard lock(registry.mutex);
		registry.flags.erase(std::remove(registry.flags.begin(), registry.flags.end(), this), registry.flags.end());
		Counters().flags.fetch_sub(1, std::memory_order_relaxed);
	}

	Common::LockGuard lock(m_mutex);

	while (m_status != Status::Set) {
		m_mutex.Unlock();
		Common::Thread::SleepMicro(10);
		m_mutex.Lock();
	}

	m_status = Status::Deleted;
	m_queue.clear();

	m_cond_var.SignalAll();

	while (m_waiting_threads > 0) {
		m_mutex.Unlock();
		Common::Thread::SleepMicro(10);
		m_mutex.Lock();
	}
}

bool KernelEventFlagPrivate::WouldMiss(bool held_at_set, uint64_t bits_now, uint64_t pattern, WaitMode wait_mode) {
	return held_at_set && !Satisfied(bits_now, pattern, wait_mode);
}

void KernelEventFlagPrivate::Enqueue(Waiter* waiter) {
	if (m_fifo) {
		m_queue.push_back(waiter);
		return;
	}
	// Behind every waiter of the same or a higher priority (as semaphore.cpp AddWaiter).
	auto it = m_queue.begin();
	while (it != m_queue.end() && (*it)->priority <= waiter->priority) {
		++it;
	}
	m_queue.insert(it, waiter);
}

void KernelEventFlagPrivate::Dequeue(Waiter* waiter) {
	m_queue.erase(std::remove(m_queue.begin(), m_queue.end(), waiter), m_queue.end());
}

void KernelEventFlagPrivate::ApplyClear(uint64_t pattern, ClearMode clear_mode) {
	switch (clear_mode) {
		case ClearMode::All: m_bits = 0; break;
		case ClearMode::Bits: m_bits &= ~pattern; break;
		case ClearMode::None: break;
	}
}

void KernelEventFlagPrivate::FillEvent(const Waiter& waiter, bool saved, MissEvent* event) const {
	std::snprintf(event->name, sizeof(event->name), "%s", m_name.c_str());
	event->pattern     = waiter.pattern;
	event->bits_at_set = waiter.bits_at_set;
	event->bits_now    = m_bits;
	event->waiters     = static_cast<uint32_t>(m_queue.size());
	event->and_mode    = (waiter.wait_mode == WaitMode::And);
	event->clear       = static_cast<uint8_t>(waiter.clear_mode);
	event->saved       = saved;
}

KernelEventFlagPrivate::Result KernelEventFlagPrivate::Wait(uint64_t bits, WaitMode wait_mode, ClearMode clear_mode,
                                                            uint64_t* result, uint32_t* ptr_micros, int priority) {
	MissEvent event {};
	bool      have_event = false;
	Result    ret        = Result::Ok;
	{
		Common::LockGuard lock(m_mutex);
		ret = (m_release == Release::AtSet
		           ? WaitAtSet(bits, wait_mode, clear_mode, result, ptr_micros, priority, &event, &have_event)
		           : WaitAtWake(bits, wait_mode, clear_mode, result, ptr_micros, priority, &event, &have_event));
	}
	if (have_event) {
		if (const auto hook = g_miss_hook.load(std::memory_order_acquire); hook != nullptr) {
			hook(event);
		}
	}
	return ret;
}

// Today's behaviour (Set wakes all, each waiter re-tests and applies its own clear), plus the queue
// that lets Set mark the waiters it satisfied.
KernelEventFlagPrivate::Result KernelEventFlagPrivate::WaitAtWake(uint64_t bits, WaitMode wait_mode,
                                                                  ClearMode clear_mode, uint64_t* result,
                                                                  uint32_t* ptr_micros, int priority,
                                                                  MissEvent* event, bool* have_event) {
	uint32_t micros     = 0;
	bool     infinitely = true;
	if (ptr_micros != nullptr) {
		micros     = *ptr_micros;
		infinitely = false;
	}

	uint32_t      elapsed = 0;
	Common::Timer t;
	t.Start();

	auto update_timeout = [&]() {
		if (ptr_micros != nullptr) {
			*ptr_micros = (elapsed >= micros ? 0 : micros - elapsed);
		}
	};

	if (m_single_thread && m_waiting_threads > 0) {
		return Result::AlreadyWaiting;
	}

	Waiter waiter;
	waiter.pattern    = bits;
	waiter.wait_mode  = wait_mode;
	waiter.clear_mode = clear_mode;
	waiter.priority   = (m_fifo ? 0 : priority);
	bool queued       = false;

	auto leave = [&]() {
		if (queued) {
			Dequeue(&waiter);
			NoteWaitTime(t);
		}
	};

	while (!Satisfied(m_bits, bits, wait_mode)) {
		if ((elapsed >= micros && !infinitely)) {
			if (result != nullptr) {
				*result = m_bits;
			}
			update_timeout();
			leave();
			return Result::TimedOut;
		}

		if (!queued) {
			waiter.start_ns = NowNs();
			Enqueue(&waiter);
			queued = true;
			Counters().waits.fetch_add(1, std::memory_order_relaxed);
		}

		m_waiting_threads++;

		if (infinitely) {
			m_cond_var.Wait(&m_mutex);
		} else {
			m_cond_var.WaitFor(&m_mutex, micros - elapsed);
		}

		m_waiting_threads--;

		elapsed = static_cast<uint32_t>(t.GetTimeS() * 1000000.0);

		switch (m_status) {
			case Status::Canceled:
				if (result != nullptr) {
					*result = m_bits;
				}
				update_timeout();
				leave();
				return Result::Canceled;
			case Status::Deleted:
				if (result != nullptr) {
					*result = m_bits;
				}
				update_timeout();
				leave();
				return Result::Deleted;
			case Status::Set: break;
		}

		if (WouldMiss(waiter.held_at_set, m_bits, bits, wait_mode)) {
			Counters().would_miss.fetch_add(1, std::memory_order_relaxed);
			FillEvent(waiter, false, event);
			*have_event = true;
		}
		waiter.held_at_set = false;
	}

	leave();

	if (result != nullptr) {
		*result = m_bits;
	}
	update_timeout();

	ApplyClear(bits, clear_mode);

	return Result::Ok;
}

// Released by Set (which applied the clear and dequeued the waiter); a release wins over timeout,
// Cancel and Delete.
KernelEventFlagPrivate::Result KernelEventFlagPrivate::WaitAtSet(uint64_t bits, WaitMode wait_mode,
                                                                 ClearMode clear_mode, uint64_t* result,
                                                                 uint32_t* ptr_micros, int priority,
                                                                 MissEvent* event, bool* have_event) {
	uint32_t micros     = 0;
	bool     infinitely = true;
	if (ptr_micros != nullptr) {
		micros     = *ptr_micros;
		infinitely = false;
	}

	uint32_t      elapsed = 0;
	Common::Timer t;
	t.Start();

	auto update_timeout = [&]() {
		if (ptr_micros != nullptr) {
			*ptr_micros = (elapsed >= micros ? 0 : micros - elapsed);
		}
	};

	if (m_single_thread && m_waiting_threads > 0) {
		return Result::AlreadyWaiting;
	}

	if (Satisfied(m_bits, bits, wait_mode)) {
		if (result != nullptr) {
			*result = m_bits;
		}
		update_timeout();
		ApplyClear(bits, clear_mode);
		return Result::Ok;
	}

	if (!infinitely && micros == 0) {
		if (result != nullptr) {
			*result = m_bits;
		}
		update_timeout();
		return Result::TimedOut;
	}

	Waiter waiter;
	waiter.pattern    = bits;
	waiter.wait_mode  = wait_mode;
	waiter.clear_mode = clear_mode;
	waiter.priority   = (m_fifo ? 0 : priority);
	waiter.start_ns   = NowNs();
	Enqueue(&waiter);
	Counters().waits.fetch_add(1, std::memory_order_relaxed);

	auto give_up = [&](Result r) {
		Dequeue(&waiter);
		if (result != nullptr) {
			*result = m_bits;
		}
		update_timeout();
		NoteWaitTime(t);
		return r;
	};

	while (!waiter.released) {
		if (elapsed >= micros && !infinitely) {
			return give_up(Result::TimedOut);
		}

		m_waiting_threads++;

		if (infinitely) {
			m_cond_var.Wait(&m_mutex);
		} else {
			m_cond_var.WaitFor(&m_mutex, micros - elapsed);
		}

		m_waiting_threads--;

		elapsed = static_cast<uint32_t>(t.GetTimeS() * 1000000.0);

		if (waiter.released) {
			break;
		}

		switch (m_status) {
			case Status::Canceled: return give_up(Result::Canceled);
			case Status::Deleted: return give_up(Result::Deleted);
			case Status::Set: break;
		}
	}

	// "Saved" is counted for no-clear waiters only: a clear-mode waiter's own clear removed its bits at
	// the Set, so its pattern says nothing here (a lower bound).
	if (clear_mode == ClearMode::None && !Satisfied(m_bits, bits, wait_mode)) {
		Counters().saved.fetch_add(1, std::memory_order_relaxed);
		FillEvent(waiter, true, event);
		*have_event = true;
	}

	if (result != nullptr) {
		*result = waiter.result;
	}
	update_timeout();
	NoteWaitTime(t);

	return Result::Ok;
}

void KernelEventFlagPrivate::WaitForSetStatus() {
	EXIT_NOT_IMPLEMENTED(m_status == Status::Deleted);

	while (m_status != Status::Set) {
		m_mutex.Unlock();
		Common::Thread::SleepMicro(10);
		m_mutex.Lock();
	}
}

void KernelEventFlagPrivate::Set(uint64_t bits) {
	Common::LockGuard lock(m_mutex);
	WaitForSetStatus();
	SetLocked(bits);
}

void KernelEventFlagPrivate::SetLocked(uint64_t bits) {
	m_bits |= bits;

	Counters().sets.fetch_add(1, std::memory_order_relaxed);
	if (m_queue.size() >= 2) {
		Counters().multi_waiter_sets.fetch_add(1, std::memory_order_relaxed);
	}

	if (m_release == Release::AtSet) {
		// In queue order: a released waiter's clear applies before the next one is tested.
		for (auto it = m_queue.begin(); it != m_queue.end();) {
			auto* waiter = *it;
			if (!Satisfied(m_bits, waiter->pattern, waiter->wait_mode)) {
				++it;
				continue;
			}
			waiter->result      = m_bits;
			waiter->bits_at_set = m_bits;
			ApplyClear(waiter->pattern, waiter->clear_mode);
			waiter->released = true;
			it               = m_queue.erase(it);
		}
	} else {
		for (auto* waiter: m_queue) {
			if (Satisfied(m_bits, waiter->pattern, waiter->wait_mode)) {
				waiter->held_at_set = true;
				waiter->bits_at_set = m_bits;
			}
		}
	}

	m_cond_var.SignalAll();
}

void KernelEventFlagPrivate::Clear(uint64_t bits) {
	Common::LockGuard lock(m_mutex);
	WaitForSetStatus();

	m_bits &= bits;
}

void KernelEventFlagPrivate::Cancel(uint64_t bits, int* num_waiting_threads) {
	Common::LockGuard lock(m_mutex);
	WaitForSetStatus();
	CancelLocked(bits, num_waiting_threads);
}

void KernelEventFlagPrivate::CancelLocked(uint64_t bits, int* num_waiting_threads) {
	if (num_waiting_threads != nullptr) {
		*num_waiting_threads = m_waiting_threads;
	}

	m_status = Status::Canceled;
	m_bits   = bits;
	m_queue.clear();

	m_cond_var.SignalAll();

	while (m_waiting_threads > 0) {
		m_mutex.Unlock();
		Common::Thread::SleepMicro(10);
		m_mutex.Lock();
	}

	m_status = Status::Set;
}

#ifdef KYTY_EVENT_FLAG_TESTS
size_t KernelEventFlagPrivate::QueuedForTest() {
	Common::LockGuard lock(m_mutex);
	return m_queue.size();
}

void KernelEventFlagPrivate::HoldThenSetForTest(uint64_t bits, uint32_t hold_micros) {
	Common::LockGuard lock(m_mutex);
	WaitForSetStatus();
	Common::Thread::SleepMicro(hold_micros);
	SetLocked(bits);
}

void KernelEventFlagPrivate::SetThenCancelForTest(uint64_t set_bits, uint64_t cancel_bits) {
	Common::LockGuard lock(m_mutex);
	WaitForSetStatus();
	SetLocked(set_bits);
	CancelLocked(cancel_bits, nullptr);
}

void KernelEventFlagPrivate::SetThenStealForTest(uint64_t bits, uint64_t steal_pattern) {
	Common::LockGuard lock(m_mutex);
	WaitForSetStatus();
	SetLocked(bits);
	if (Satisfied(m_bits, steal_pattern, WaitMode::And)) {
		ApplyClear(steal_pattern, ClearMode::Bits);
	}
}
#endif

void KernelEventFlagPrivate::Describe(char* text, size_t size, size_t* used, uint64_t now_ns) const {
	Append(text, size, used, " %s bits=0x%llx [", m_name.c_str(), static_cast<unsigned long long>(m_bits));
	bool first = true;
	for (const auto* waiter: m_queue) {
		const uint64_t age_ms = (now_ns > waiter->start_ns ? (now_ns - waiter->start_ns) / 1000000u : 0);
		Append(text, size, used, "%s0x%llx %s clr=%d %llums", first ? "" : ", ",
		       static_cast<unsigned long long>(waiter->pattern), waiter->wait_mode == WaitMode::And ? "and" : "or",
		       static_cast<int>(waiter->clear_mode), static_cast<unsigned long long>(age_ms));
		first = false;
	}
	Append(text, size, used, "]");
}

size_t DescribeWaitingFlags(char* text, size_t size) {
	if (text == nullptr || size == 0) {
		return 0;
	}
	text[0]     = '\0';
	size_t used = 0;
	auto&  registry = GetRegistry();
	if (!registry.mutex.TryLock()) {
		Append(text, size, &used, "evflags: busy");
		return used;
	}
	Append(text, size, &used, "evflags:");
	const uint64_t now     = NowNs();
	size_t         waiting = 0;
	for (auto* flag: registry.flags) {
		// The name is fixed after construction, so it is read without the flag's lock.
		if (!flag->m_mutex.TryLock()) {
			Append(text, size, &used, " %s:busy", flag->m_name.c_str());
			continue;
		}
		if (!flag->m_queue.empty()) {
			flag->Describe(text, size, &used, now);
			waiting++;
		}
		flag->m_mutex.Unlock();
	}
	registry.mutex.Unlock();
	if (waiting == 0) {
		Append(text, size, &used, " none waiting");
	}
	return used;
}

} // namespace Libs::LibKernel::EventFlag
