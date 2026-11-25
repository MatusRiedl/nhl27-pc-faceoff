#ifndef KYTY_COMMON_FREEZE_DUMP_H_
#define KYTY_COMMON_FREEZE_DUMP_H_

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace Common::FreezeDump {

// NHL27FREEZE (KYTY_FREEZE_DUMP, default 1): when the game stops flipping for 45 s while the
// emulated display keeps running (vblanks advance, so the emulator is not paused) and the window
// is not minimized, or when the game calls Coredump, print one NHL27FREEZE line and write
// freeze_<time>.dmp (thread stacks, a few MB) into the working folder. At most 2 dumps per
// session; play goes on (the game may pause for the second or two a dump takes). Open the dump
// in WinDbg or Visual Studio with the build's PDB.
// The dump is written by this exe started again as a helper (kHelperArg), so a lock a stuck
// thread holds cannot block it.
inline constexpr char kHelperArg[] = "--nhl27-freeze-dump";

// The helper: `<exe> --nhl27-freeze-dump <pid> <file>`. Main() calls it before any start-up and
// returns its result (0, or the Windows error code).
int HelperMain(int argc, char* argv[]) noexcept;

// At every vblank, for each open video-out handle: its vblank count and the guest's flip count.
// Only handle 1 is kept (the game's display; NHL27FPS shows no other).
void NoteVblank(int handle, uint64_t vblanks, uint64_t flips) noexcept;

// The game window is minimized (SDL window events). The presenter reads it too.
inline std::atomic<bool> g_minimized {false};
inline void SetMinimized(bool minimized) noexcept {
	g_minimized.store(minimized, std::memory_order_relaxed);
}
[[nodiscard]] inline bool IsMinimized() noexcept {
	return g_minimized.load(std::memory_order_relaxed);
}

// Writes the emulator part of the NHL27FREEZE line (lock-free reads only: the thread holding a
// lock may be the frozen one). SetStateProvider(nullptr) returns only once no call is running, so
// what the provider reads can be destroyed right after it.
using StateFn = void (*)(char* text, size_t size);
void SetStateProvider(StateFn state) noexcept;

// A second provider for a "NHL27FREEZE: detail ..." line after the NHL27FREEZE line (the event flags
// with waiting threads). Same rules: no blocking locks, and it must stay valid until replaced.
void SetDetailProvider(StateFn detail) noexcept;

// Starts the watchdog thread; nothing when KYTY_FREEZE_DUMP=0 or off Windows.
void Start() noexcept;

// The game called Coredump: the first call dumps and waits up to 10 s for the dump.
void OnGameCoredump(const char* thread_name) noexcept;

} // namespace Common::FreezeDump

#endif // KYTY_COMMON_FREEZE_DUMP_H_
