#include "kernel/eventFlag.h"

#include "common/assert.h"
#include "common/common.h"
#include "common/freezeDump.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "kernel/eventFlagCore.h"
#include "libs/errno.h"
#include "libs/libs.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace Libs::LibKernel::EventFlag {

LIB_NAME("libkernel", "libkernel");

namespace {

struct EventFlagWaitMode {
	KernelEventFlagPrivate::WaitMode  wait  = KernelEventFlagPrivate::WaitMode::And;
	KernelEventFlagPrivate::ClearMode clear = KernelEventFlagPrivate::ClearMode::None;
};

EventFlagWaitMode DecodeEventFlagWaitMode(uint32_t wait_mode) {
	EventFlagWaitMode mode;

	switch (wait_mode & 0xfu) {
		case 0x01: mode.wait = KernelEventFlagPrivate::WaitMode::And; break;
		case 0x02: mode.wait = KernelEventFlagPrivate::WaitMode::Or; break;
		default: EXIT("unknown mode: %u\n", wait_mode);
	}

	switch (wait_mode & 0xf0u) {
		case 0x00: mode.clear = KernelEventFlagPrivate::ClearMode::None; break;
		case 0x10: mode.clear = KernelEventFlagPrivate::ClearMode::All; break;
		case 0x20: mode.clear = KernelEventFlagPrivate::ClearMode::Bits; break;
		default: EXIT("unknown mode: %u\n", wait_mode);
	}

	return mode;
}

// NHL27EVMISS: the first 20 would-miss (KYTY_EVFLAG_RELEASE_AT_SET=0) or saved (=1) releases.
std::atomic<int> g_miss_lines {0};
constexpr int    kMissLines = 20;

void PrintMiss(const MissEvent& e) {
	if (g_miss_lines.fetch_add(1, std::memory_order_relaxed) >= kMissLines) {
		return;
	}
	std::printf("NHL27EVMISS: %s flag=%s pattern=0x%llx mode=%s clear=%d bits_at_set=0x%llx bits_now=0x%llx "
	            "waiters=%u\n",
	            e.saved ? "saved" : "would_miss", e.name, static_cast<unsigned long long>(e.pattern),
	            e.and_mode ? "and" : "or", static_cast<int>(e.clear), static_cast<unsigned long long>(e.bits_at_set),
	            static_cast<unsigned long long>(e.bits_now), e.waiters);
	std::fflush(stdout);
}

void WriteFreezeDetail(char* text, size_t size) {
	DescribeWaitingFlags(text, size);
}

// KYTY_EVFLAG_RELEASE_AT_SET, read once at the first create ("1" = release at Set; anything else = today's
// release at wake). Also installs the miss line and the freeze-report detail.
KernelEventFlagPrivate::Release StartRelease() {
	static const KernelEventFlagPrivate::Release release = [] {
		const char* value  = std::getenv("KYTY_EVFLAG_RELEASE_AT_SET");
		const bool  at_set = (value != nullptr && value[0] == '1');
		SetMissHook(&PrintMiss);
		Common::FreezeDump::SetDetailProvider(&WriteFreezeDetail);
		std::printf("NHL27EVFLAG: release_at_set=%d\n", at_set ? 1 : 0);
		std::fflush(stdout);
		return at_set ? KernelEventFlagPrivate::Release::AtSet : KernelEventFlagPrivate::Release::AtWake;
	}();
	return release;
}

} // namespace

void PrintEventFlagStats() {
	static auto last = std::chrono::steady_clock::now();
	const auto  now  = std::chrono::steady_clock::now();
	if (now - last < std::chrono::seconds(30)) {
		return;
	}
	last    = now;
	auto& c = Counters();
	auto  total = [](std::atomic<uint64_t>& slot) {
		return static_cast<unsigned long long>(slot.load(std::memory_order_relaxed));
	};
	std::printf("NHL27EVFLAG: flags=%llu waits=%llu sets=%llu multi_waiter_sets=%llu would_miss=%llu saved=%llu "
	            "longest_wait_ms=%.1f\n",
	            total(c.flags), total(c.waits), total(c.sets), total(c.multi_waiter_sets), total(c.would_miss),
	            total(c.saved), static_cast<double>(c.longest_wait_us.exchange(0, std::memory_order_relaxed)) / 1000.0);
}

int KYTY_SYSV_ABI KernelCreateEventFlag(KernelEventFlag* ef, const char* name, uint32_t attr,
                                        uint64_t init_pattern, const void* param) {
	PRINT_NAME();

	if (ef == nullptr || name == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	if (param != nullptr || (attr & ~0x33u) != 0) {
		return KERNEL_ERROR_EINVAL;
	}

	bool single = true;
	bool fifo   = true;

	switch (attr & 0x0fu) {
		case 0x00:
		case 0x01: fifo = true; break;
		case 0x02: fifo = false; break;
		default: return KERNEL_ERROR_EINVAL;
	}

	switch (attr & 0xf0u) {
		case 0x00:
		case 0x10: single = true; break;
		case 0x20: single = false; break;
		default: return KERNEL_ERROR_EINVAL;
	}

	*ef = new KernelEventFlagPrivate(std::string(name), single, fifo, init_pattern, StartRelease());

	LOGF("\tEventFlag create: %s\n", name);

	return OK;
}

int KYTY_SYSV_ABI KernelDeleteEventFlag(KernelEventFlag ef) {
	PRINT_NAME();

	if (ef == nullptr) {
		return KERNEL_ERROR_ESRCH;
	}

	delete ef;

	return OK;
}

int KYTY_SYSV_ABI KernelWaitEventFlag(KernelEventFlag ef, uint64_t bit_pattern, uint32_t wait_mode,
                                      uint64_t* result_pat, KernelUseconds* timeout) {
	PRINT_NAME();

	if (ef == nullptr) {
		return KERNEL_ERROR_ESRCH;
	}

	if (bit_pattern == 0) {
		return KERNEL_ERROR_EINVAL;
	}

	const auto mode     = DecodeEventFlagWaitMode(wait_mode);
	const int  priority = (ef->Fifo() ? 0 : PthreadGetCurrentPriorityForKernel());
	auto       result   = ef->Wait(bit_pattern, mode.wait, mode.clear, result_pat, timeout, priority);

	int ret = OK;

	switch (result) {
		case KernelEventFlagPrivate::Result::Ok: ret = OK; break;
		case KernelEventFlagPrivate::Result::AlreadyWaiting: ret = KERNEL_ERROR_EPERM; break;
		case KernelEventFlagPrivate::Result::TimedOut: ret = KERNEL_ERROR_ETIMEDOUT; break;
		case KernelEventFlagPrivate::Result::Canceled: ret = KERNEL_ERROR_ECANCELED; break;
		case KernelEventFlagPrivate::Result::Deleted: ret = KERNEL_ERROR_EACCES; break;
	}

	return ret;
}

int KYTY_SYSV_ABI KernelPollEventFlag(KernelEventFlag ef, uint64_t bit_pattern, uint32_t wait_mode,
                                      uint64_t* result_pat) {
	PRINT_NAME();

	if (ef == nullptr) {
		return KERNEL_ERROR_ESRCH;
	}

	if (bit_pattern == 0) {
		return KERNEL_ERROR_EINVAL;
	}

	const auto mode   = DecodeEventFlagWaitMode(wait_mode);
	auto       result = ef->Poll(bit_pattern, mode.wait, mode.clear, result_pat);

	int ret = OK;

	switch (result) {
		case KernelEventFlagPrivate::Result::Ok: ret = OK; break;
		case KernelEventFlagPrivate::Result::AlreadyWaiting: ret = KERNEL_ERROR_EPERM; break;
		case KernelEventFlagPrivate::Result::TimedOut:
		case KernelEventFlagPrivate::Result::Canceled:
		case KernelEventFlagPrivate::Result::Deleted: ret = KERNEL_ERROR_EBUSY; break;
	}

	return ret;
}

int KYTY_SYSV_ABI KernelSetEventFlag(KernelEventFlag ef, uint64_t bit_pattern) {
	PRINT_NAME();

	if (ef == nullptr) {
		return KERNEL_ERROR_ESRCH;
	}

	ef->Set(bit_pattern);

	return OK;
}

int KYTY_SYSV_ABI KernelClearEventFlag(KernelEventFlag ef, uint64_t bit_pattern) {
	PRINT_NAME();

	if (ef == nullptr) {
		return KERNEL_ERROR_ESRCH;
	}

	ef->Clear(bit_pattern);

	return OK;
}

int KYTY_SYSV_ABI KernelCancelEventFlag(KernelEventFlag ef, uint64_t set_pattern,
                                        int* num_wait_threads) {
	PRINT_NAME();

	if (ef == nullptr) {
		return KERNEL_ERROR_ESRCH;
	}

	ef->Cancel(set_pattern, num_wait_threads);

	return OK;
}

} // namespace Libs::LibKernel::EventFlag
