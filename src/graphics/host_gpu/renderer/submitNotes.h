#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SUBMITNOTES_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SUBMITNOTES_H_

// Freeze diagnostics: the last queue submissions of each scheduler and of the DLSS bridge, with
// the semaphore values they wait for and signal. The NHL27FREEZE report prints the ones the GPU
// has not finished (NHL27SUBMIT lines), so a freeze where submitted work never completes names
// the wait it is stuck on. Stores only (relaxed atomics, no branches): the submit paths keep
// their PGO profiles. Handles are stored as integers.

#include <array>
#include <atomic>
#include <cstdint>

namespace Libs::Graphics::SubmitNotes {

struct Note {
	std::atomic<uint64_t> tick {0};
	std::atomic<uint64_t> wait_semaphore[2] {};
	std::atomic<uint64_t> wait_value[2] {};
	std::atomic<uint64_t> signal_semaphore {0};
	std::atomic<uint64_t> signal_value {0};
	// Wait count in the low byte, signal count in the next one.
	std::atomic<uint32_t> counts {0};
};

constexpr uint64_t kRing = 16;

struct Ring {
	std::array<Note, kRing> notes;
};

// A direct submission outside the schedulers (the Vulkan DLSS bridge): it waits for
// `wait_value` and signals `signal_value` on `semaphore`. `count` = submissions made.
struct External {
	std::atomic<uint64_t> semaphore {0};
	std::atomic<uint64_t> wait_value {0};
	std::atomic<uint64_t> signal_value {0};
	std::atomic<uint64_t> count {0};
};
inline External g_dlss;

inline void NoteExternal(External& note, uint64_t semaphore, uint64_t wait_value, uint64_t signal_value) noexcept {
	note.semaphore.store(semaphore, std::memory_order_relaxed);
	note.wait_value.store(wait_value, std::memory_order_relaxed);
	note.signal_value.store(signal_value, std::memory_order_relaxed);
	note.count.fetch_add(1, std::memory_order_relaxed);
}

} // namespace Libs::Graphics::SubmitNotes

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SUBMITNOTES_H_ */
