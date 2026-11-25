#include "common/freezeDump.h"

#include "common/config.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <iterator>
#include <mutex>
#include <thread>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // IWYU pragma: keep
// clang-format off
#include <dbghelp.h>
#include <tlhelp32.h>
// clang-format on
#endif

namespace Common::FreezeDump {

namespace {

std::atomic<StateFn>  g_state {nullptr};
std::atomic<StateFn>  g_detail {nullptr};
std::atomic<uint64_t> g_vblanks {0};
std::atomic<uint64_t> g_flips {0};

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
// Held around each state-provider call and each provider change. An SRW lock has no destructor,
// so the detached watchdog can still take it while the process exits.
SRWLOCK g_state_lock = SRWLOCK_INIT;
#endif

[[nodiscard]] bool Enabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_FREEZE_DUMP");
		return value == nullptr || value[0] != '0';
	}();
	return enabled;
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

// 45 one-second polls in a row with no new flip. Polls are counted, not timed, so a PC that
// sleeps and resumes does not count the sleep. Kept play logs have healthy gaps of up to 26 s with
// no flip (boot, online connect); a real freeze lasts minutes, and the game itself calls Coredump
// about 20 s into one (the first trigger).
constexpr uint32_t kFreezePolls = 45;
constexpr uint32_t kMaxDumps    = 2;
// How long the watchdog waits for the dump helper (a dump takes seconds).
constexpr DWORD kHelperWaitMs = 120000;
constexpr size_t kExeChars    = 2048;

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

// Never destroyed: the watchdog thread may still wait on it while the process exits.
struct Watch {
	std::mutex              mutex;
	std::condition_variable wake;
	std::condition_variable dumped;
	bool                    coredump_requested = false;
	char                    coredump_thread[64] {};
	uint32_t                dumps              = 0;
	uint32_t                finished           = 0;
	wchar_t                 exe[kExeChars] {}; // this exe: the dump helper
};
Watch* g_watch = nullptr;

// Helper side. The stack pointers MiniDumpWriteDump reports, and how far each stack's memory
// region goes: the memory callback adds up to 64 KB above each one, so threads running on stacks
// the emulator made (outside the stack range Windows knows) keep their frames.
constexpr size_t   kMaxStacks  = 2048;
constexpr uint64_t kStackBytes = 64 * 1024;
struct StackRange {
	uint64_t base;
	uint32_t size;
};
StackRange g_stacks[kMaxStacks];
size_t     g_stack_count = 0;
size_t     g_stack_next  = 0;

void NoteStack(HANDLE process, uint64_t sp) {
	if (sp == 0 || g_stack_count >= kMaxStacks) {
		return;
	}
	MEMORY_BASIC_INFORMATION info {};
	if (VirtualQueryEx(process, reinterpret_cast<const void*>(sp), &info, sizeof(info)) == 0 ||
	    info.State != MEM_COMMIT) {
		return;
	}
	const uint64_t region_end = reinterpret_cast<uint64_t>(info.BaseAddress) + info.RegionSize;
	const uint64_t base       = sp & ~uint64_t {0xF};
	const uint64_t end        = base + kStackBytes < region_end ? base + kStackBytes : region_end;
	if (end > base) {
		g_stacks[g_stack_count++] = {base, static_cast<uint32_t>(end - base)};
	}
}

BOOL CALLBACK DumpCallback(PVOID param, PMINIDUMP_CALLBACK_INPUT input, PMINIDUMP_CALLBACK_OUTPUT output) {
	if (input == nullptr || output == nullptr) {
		return TRUE;
	}
	const auto process = static_cast<HANDLE>(param);
	switch (input->CallbackType) {
		case ThreadCallback: NoteStack(process, input->Thread.Context.Rsp); return TRUE;
		case ThreadExCallback: NoteStack(process, input->ThreadEx.Context.Rsp); return TRUE;
		case MemoryCallback:
			if (g_stack_next < g_stack_count) {
				output->MemoryBase = g_stacks[g_stack_next].base;
				output->MemorySize = g_stacks[g_stack_next].size;
				g_stack_next++;
				return TRUE;
			}
			return FALSE;
		default: return TRUE;
	}
}

uint32_t CountThreads() {
	const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (snapshot == INVALID_HANDLE_VALUE) {
		return 0;
	}
	const DWORD   pid   = GetCurrentProcessId();
	uint32_t      count = 0;
	THREADENTRY32 entry {};
	entry.dwSize = sizeof(entry);
	for (BOOL more = Thread32First(snapshot, &entry); more != FALSE; more = Thread32Next(snapshot, &entry)) {
		count += entry.th32OwnerProcessID == pid ? 1u : 0u;
	}
	CloseHandle(snapshot);
	return count;
}

// On the watchdog thread. The dump is written by a second process (this exe in helper mode): an
// in-process MiniDumpWriteDump suspends every other thread and then can block for good on a lock
// one of them holds (the loader lock, seen in a harness). The NHL27FREEZE line goes out first, so
// the log has it whatever the helper does.
void WriteDump(Watch& watch, const char* reason, const char* detail) {
	SYSTEMTIME time {};
	GetLocalTime(&time);
	char name[64];
	std::snprintf(name, sizeof(name), "freeze_%04u%02u%02u_%02u%02u%02u.dmp", time.wYear, time.wMonth, time.wDay,
	              time.wHour, time.wMinute, time.wSecond);
	char state[256] = "";
	char more[1536] = "";
	AcquireSRWLockExclusive(&g_state_lock);
	if (const auto fn = g_state.load(std::memory_order_acquire); fn != nullptr) {
		fn(state, sizeof(state));
	}
	if (const auto fn = g_detail.load(std::memory_order_acquire); fn != nullptr) {
		fn(more, sizeof(more));
	}
	ReleaseSRWLockExclusive(&g_state_lock);
	std::printf("NHL27FREEZE: reason=%s%s flips=%llu vblanks=%llu threads=%u %s dump=%s\n", reason, detail,
	            static_cast<unsigned long long>(g_flips.load(std::memory_order_relaxed)),
	            static_cast<unsigned long long>(g_vblanks.load(std::memory_order_relaxed)), CountThreads(), state,
	            name);
	if (more[0] != '\0') {
		std::printf("NHL27FREEZE: detail %s\n", more);
	}
	std::fflush(stdout);
	if (watch.exe[0] == L'\0') {
		std::printf("NHL27FREEZE: no dump (exe path unknown)\n");
		std::fflush(stdout);
		return;
	}
	wchar_t command[kExeChars + 128];
	std::swprintf(command, std::size(command), L"\"%ls\" %hs %lu %hs", watch.exe, kHelperArg, GetCurrentProcessId(),
	              name);
	STARTUPINFOW        startup {};
	PROCESS_INFORMATION process {};
	startup.cb = sizeof(startup);
	// No inherited handles, no console window; the helper inherits this working folder.
	if (CreateProcessW(watch.exe, command, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
	                   &process) == FALSE) {
		std::printf("NHL27FREEZE: dump helper did not start (error %lu)\n", GetLastError());
		std::fflush(stdout);
		return;
	}
	CloseHandle(process.hThread);
	const DWORD waited = WaitForSingleObject(process.hProcess, kHelperWaitMs);
	DWORD       code   = STILL_ACTIVE;
	if (waited == WAIT_OBJECT_0) {
		GetExitCodeProcess(process.hProcess, &code);
	} else {
		TerminateProcess(process.hProcess, 1);
	}
	CloseHandle(process.hProcess);
	WIN32_FILE_ATTRIBUTE_DATA file {};
	const uint64_t            bytes = GetFileAttributesExA(name, GetFileExInfoStandard, &file) != FALSE
	                                      ? (static_cast<uint64_t>(file.nFileSizeHigh) << 32u) | file.nFileSizeLow
	                                      : 0;
	std::printf("NHL27FREEZE: dump=%s ok=%d error=0x%lx bytes=%llu%s\n", name,
	            waited == WAIT_OBJECT_0 && code == 0 ? 1 : 0, code, static_cast<unsigned long long>(bytes),
	            waited == WAIT_OBJECT_0 ? "" : " (helper timed out)");
	std::fflush(stdout);
}

void WatchThread(Watch* watch_ptr) {
	auto&    watch       = *watch_ptr;
	uint64_t last_flips  = 0;
	uint64_t last_vblank = 0;
	uint32_t still       = 0;
	for (;;) {
		std::unique_lock lock(watch.mutex);
		watch.wake.wait_for(lock, std::chrono::seconds(1), [&] { return watch.coredump_requested; });
		if (watch.coredump_requested) {
			watch.coredump_requested = false;
			if (watch.dumps < kMaxDumps) {
				watch.dumps++;
				char detail[96];
				std::snprintf(detail, sizeof(detail), " thread=%s", watch.coredump_thread);
				lock.unlock();
				WriteDump(watch, "game_coredump", detail);
				lock.lock();
			}
			watch.finished++;
			watch.dumped.notify_all();
			continue;
		}
		lock.unlock();
		const uint64_t flips  = g_flips.load(std::memory_order_relaxed);
		const uint64_t vblank = g_vblanks.load(std::memory_order_relaxed);
		// A freeze: the display runs (not paused), the game has flipped before, and no new flip.
		if (flips != last_flips) {
			still = 0;
		} else if (flips != 0 && vblank != last_vblank && !IsMinimized()) {
			still++;
		}
		last_flips  = flips;
		last_vblank = vblank;
		if (still == kFreezePolls) {
			lock.lock();
			const bool dump = watch.dumps < kMaxDumps;
			watch.dumps += dump ? 1u : 0u;
			lock.unlock();
			if (dump) {
				WriteDump(watch, "no_flip_45s", "");
			}
		}
	}
}

#endif

} // namespace

void NoteVblank(int handle, uint64_t vblanks, uint64_t flips) noexcept {
	if (handle == 1) {
		g_vblanks.store(vblanks, std::memory_order_relaxed);
		g_flips.store(flips, std::memory_order_relaxed);
	}
}

void SetStateProvider(StateFn state) noexcept {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	AcquireSRWLockExclusive(&g_state_lock);
	g_state.store(state, std::memory_order_release);
	ReleaseSRWLockExclusive(&g_state_lock);
#else
	g_state.store(state, std::memory_order_release);
#endif
}

void SetDetailProvider(StateFn detail) noexcept {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	AcquireSRWLockExclusive(&g_state_lock);
	g_detail.store(detail, std::memory_order_release);
	ReleaseSRWLockExclusive(&g_state_lock);
#else
	g_detail.store(detail, std::memory_order_release);
#endif
}

void Start() noexcept {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (!Enabled() || g_watch != nullptr) {
		return;
	}
	// noexcept (its callers keep their PGO profiles): a watchdog that cannot start leaves play as it is.
	try {
		auto*       watch  = new Watch;
		const DWORD length = GetModuleFileNameW(nullptr, watch->exe, static_cast<DWORD>(kExeChars));
		if (length == 0 || length >= kExeChars) {
			watch->exe[0] = L'\0';
		}
		std::thread(WatchThread, watch).detach();
		g_watch = watch;
	} catch (...) {
		std::printf("NHL27FREEZE: watchdog not started\n");
		std::fflush(stdout);
	}
#endif
}

void OnGameCoredump(const char* thread_name) noexcept {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	static std::atomic<bool> asked {false};
	if (g_watch == nullptr || asked.exchange(true)) {
		return;
	}
	auto&            watch = *g_watch;
	std::unique_lock lock(watch.mutex);
	const uint32_t   target = watch.finished + 1;
	std::snprintf(watch.coredump_thread, sizeof(watch.coredump_thread), "%s",
	              thread_name != nullptr ? thread_name : "?");
	watch.coredump_requested = true;
	watch.wake.notify_all();
	watch.dumped.wait_for(lock, std::chrono::seconds(10), [&] { return watch.finished >= target; });
#else
	(void)thread_name;
#endif
}

int HelperMain(int argc, char* argv[]) noexcept {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	// kyty_emulator.exe --nhl27-freeze-dump <pid> <file>: returns 0 or the error code.
	if (argc < 4) {
		return ERROR_BAD_ARGUMENTS;
	}
	const auto   pid     = static_cast<DWORD>(std::strtoul(argv[2], nullptr, 10));
	const HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
	if (process == nullptr) {
		return static_cast<int>(GetLastError());
	}
	MiniDumpWriteDumpFn write = nullptr;
	if (const HMODULE dbghelp = LoadLibraryExA("dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	    dbghelp != nullptr) {
		write = reinterpret_cast<MiniDumpWriteDumpFn>(
		    reinterpret_cast<void*>(GetProcAddress(dbghelp, "MiniDumpWriteDump")));
	}
	if (write == nullptr) {
		const DWORD error = GetLastError();
		CloseHandle(process);
		return static_cast<int>(error != 0 ? error : ERROR_PROC_NOT_FOUND);
	}
	const HANDLE file = CreateFileA(argv[3], GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		const DWORD error = GetLastError();
		CloseHandle(process);
		return static_cast<int>(error);
	}
	MINIDUMP_CALLBACK_INFORMATION callback {};
	callback.CallbackRoutine = DumpCallback;
	callback.CallbackParam   = process;
	const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
	DWORD      error = 0;
	// Up to 3 tries: a module loading or unloading during the dump fails it (0x80070018).
	for (int attempt = 0; attempt < 3; attempt++) {
		g_stack_count = 0;
		g_stack_next  = 0;
		SetFilePointer(file, 0, nullptr, FILE_BEGIN);
		SetEndOfFile(file);
		if (write(process, pid, file, type, nullptr, nullptr, &callback) != FALSE) {
			error = 0;
			break;
		}
		error = GetLastError();
		error = error != 0 ? error : ERROR_GEN_FAILURE;
	}
	CloseHandle(file);
	CloseHandle(process);
	return static_cast<int>(error);
#else
	(void)argc;
	(void)argv;
	return 1;
#endif
}

} // namespace Common::FreezeDump
