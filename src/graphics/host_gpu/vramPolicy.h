#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VRAMPOLICY_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VRAMPOLICY_H_

// What happens when video memory runs out (NHL27VRAM lines). NHL 27 uses 6-8 GB of video memory in a match, so a
// 6 GB card fills up; the texture collector cannot free the textures the game keeps in its texture heaps.
//
// KYTY_VRAM_PAGEABLE (default 1): enable VK_EXT_pageable_device_local_memory (with VK_EXT_memory_priority, which it
// requires) when the driver offers both, so the driver moves idle video memory to system memory when VRAM is full
// instead of refusing allocations. 0 leaves both off.
// KYTY_DBG_VRAM_RESERVE_MB=N (tests only, default 0): hold N MB of video memory from start-up, at the highest memory
// priority, to reproduce a smaller card (10000 on a 16 GB card leaves about what a 6 GB card has).
// Images that video memory refuses go to system memory (vma.cpp, CreateImageOrSpill) instead of ending the run.

#include <cstdint>

namespace Libs::Graphics::Vram {

[[nodiscard]] bool PageableWanted() noexcept;

// Device creation: VK_EXT_memory_priority's feature is on, so allocations may carry a priority.
void SetPriorityEnabled(bool enabled) noexcept;
[[nodiscard]] bool PriorityEnabled() noexcept;

} // namespace Libs::Graphics::Vram

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VRAMPOLICY_H_ */
