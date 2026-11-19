#pragma once

// DLSS frame generation through NVIDIA Streamline (v0.30.0, experimental, off by default).
// KYTY_DLSSG=1 with KYTY_DLSS=1 in a build with KYTY_ENABLE_DLSSG, and the six Streamline DLLs next to
// the exe (frameGenPolicy.h RequiredDlls). Streamline's sl.interposer.dll is then the Vulkan loader:
// its vkGetInstanceProcAddr feeds the dispatcher, so the instance, device, swapchain and present go
// through it. Without the switch, or in other builds, every function here does nothing and the
// emulator runs exactly as before. Notes: docs/notes/streamline-dlssg.md (PS5 Emulator repo).
//
// Threads: LoaderBeforeInstance .. AfterDevice and Shutdown on the window thread; TagUpscale on the
// GPU thread (upscale pass); FlipSubmitted on the flip-submit thread; the present functions on the
// present thread.

#include "graphics/host_gpu/vulkanCommon.h" // the project's Vulkan macros before vulkan.h

#include <array>
#include <cstdint>

struct SDL_Window;
typedef struct VmaAllocation_T* VmaAllocation;

namespace Libs::Graphics {
struct GraphicContext;
struct Nhl27Fsr3Constants;
} // namespace Libs::Graphics

namespace Libs::Graphics::FrameGen {

// One render-size input of the upscale pass, in `layout` at the point of the tag in the command buffer.
struct TagImage {
	VkImage           image      = VK_NULL_HANDLE;
	VmaAllocation     allocation = nullptr;
	VkFormat          format     = VK_FORMAT_UNDEFINED;
	uint32_t          width = 0, height = 0;
	VkImageLayout     layout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkImageUsageFlags usage  = 0;
};

// A frame's tag slot, taken at record time: its frame index (0 = no tags) and the camera then
// (world-to-view, row by row, row vectors; identity until the camera scan found one).
struct TagReservation {
	uint64_t              index = 0;
	std::array<float, 16> world_to_view {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

struct PresentStep {
	bool recreate = false; // recreate the swapchain (frame generation turning on or off) before presenting
};

#if KYTY_ENABLE_DLSSG

// Window thread, before the Vulkan instance: decides the loader. Returns Streamline's
// vkGetInstanceProcAddr, or `plain` (SDL's) when frame generation is off or anything failed.
PFN_vkGetInstanceProcAddr LoaderBeforeInstance(PFN_vkGetInstanceProcAddr plain);
// Streamline is the loader for this session.
bool Active();
// The window surface through the dispatcher (Streamline hooks surface creation), or VK_NULL_HANDLE
// when Streamline is not active or creation failed: the caller then uses SDL.
VkSurfaceKHR CreateSurface(SDL_Window* window, VkInstance instance);
// Queue flags no extra (readback) queue family may have: the optical-flow family stays Streamline's.
VkQueueFlags RejectedQueueFlags();
// After the device and VULKAN_HPP_DEFAULT_DISPATCHER.init(device): points the hooked dispatcher
// entries at the interposer, checks the adapter, resolves the feature functions.
void AfterDevice(GraphicContext& graphics);
// Before the device is destroyed (after waiting for it).
void Shutdown(VkDevice device);

// Swapchain creation (present thread): the present mode while frame generation is on.
VkPresentModeKHR AdjustPresentMode(VkPresentModeKHR requested, bool mailbox_available, bool immediate_available);
// GPU thread, FlipQueue::Prepare (in GPU order): the flip shows the frame tagged since the last flip.
void FlipPrepared(uint64_t request_id);
// Present thread, right before a flip is presented (the group's request id). A present without it
// (blank frame, last-frame repeat) carries no new frame.
void PresentingFlip(uint64_t request_id);
// Present thread, once per present before acquiring.
PresentStep BeforePresent(uint32_t width, uint32_t height);
// After a recreate BeforePresent asked for; true when one more recreate is needed at once.
bool AfterFrameGenRecreate();
// Before a recreate for another reason (resize, out of date, surface lost, HDR change).
void BeforeOutsideRecreate();
// Around vkQueuePresentKHR.
void PresentStart();
void PresentEnd();

// A guest flip was submitted (end of a game frame, start of the next). guest_thread: called on the
// thread that submitted it (the CPU flip path), where Reflex may sleep; the GPU path passes false.
void FlipSubmitted(bool guest_thread);

// GPU thread, upscale pass (beside the DLSS evaluation), at record time: the frame index this pass's
// tags carry (0 for no tags this frame) and the camera for it. Taken in GPU-thread order, so the flip
// that follows binds it (FlipPrepared) even when the pass's commands are replayed later on the
// recording thread.
TagReservation ReserveTag(const Nhl27Fsr3Constants& constants);
// Where the pass's commands reach a Vulkan command buffer (CommandRecorder::RecordExternal: at once,
// or at replay on the recording thread): tags depth and motion vectors and sets the constants for
// the reserved frame. Records Streamline commands into `command` (the caller invalidates its
// bound-state cache afterwards). Returns true when it may have recorded anything.
bool TagUpscale(GraphicContext& graphics, VkCommandBuffer command, const TagImage& depth, const TagImage& motion,
                const Nhl27Fsr3Constants& constants, bool reset, const TagReservation& reservation);

// KYTY_DLSSG_CAMERA_SCAN=1 (works without Streamline loaded): finds the game's camera in the draws'
// constant buffers (frameGenMath.h CameraTracker), logs NHL27CAMSCAN lines (hashes, slots, offsets,
// scores only) and gives the view to the tags. GPU thread. Read the switch once per call site.
bool CameraScanEnabled();
// The upscale pass's constants as captured (any upscaler): the projection the scan looks for.
void CameraScanConstants(const Nhl27Fsr3Constants& constants);
// One buffer of a draw's vertex shader: `slot` is its descriptor index, `address` / `size` guest.
void CameraScanBuffer(uint64_t shader_hash, uint32_t slot, uint64_t address, uint64_t size);

#else

inline PFN_vkGetInstanceProcAddr LoaderBeforeInstance(PFN_vkGetInstanceProcAddr plain) { return plain; }
inline bool                      Active() { return false; }
inline VkSurfaceKHR              CreateSurface(SDL_Window* /*window*/, VkInstance /*instance*/) { return VK_NULL_HANDLE; }
inline VkQueueFlags              RejectedQueueFlags() { return 0; }
inline void                      AfterDevice(GraphicContext& /*graphics*/) {}
inline void                      Shutdown(VkDevice /*device*/) {}
inline VkPresentModeKHR AdjustPresentMode(VkPresentModeKHR requested, bool /*mailbox*/, bool /*immediate*/) {
	return requested;
}
inline void        FlipPrepared(uint64_t /*request_id*/) {}
inline void        PresentingFlip(uint64_t /*request_id*/) {}
inline PresentStep BeforePresent(uint32_t /*width*/, uint32_t /*height*/) { return {}; }
inline bool        AfterFrameGenRecreate() { return false; }
inline void        BeforeOutsideRecreate() {}
inline void        PresentStart() {}
inline void        PresentEnd() {}
inline void        FlipSubmitted(bool /*guest_thread*/) {}
inline TagReservation ReserveTag(const Nhl27Fsr3Constants& /*constants*/) { return {}; }
inline bool TagUpscale(GraphicContext& /*graphics*/, VkCommandBuffer /*command*/, const TagImage& /*depth*/,
                       const TagImage& /*motion*/, const Nhl27Fsr3Constants& /*constants*/, bool /*reset*/,
                       const TagReservation& /*reservation*/) {
	return false;
}
inline bool CameraScanEnabled() { return false; }
inline void CameraScanConstants(const Nhl27Fsr3Constants& /*constants*/) {}
inline void CameraScanBuffer(uint64_t /*shader_hash*/, uint32_t /*slot*/, uint64_t /*address*/, uint64_t /*size*/) {}

#endif

} // namespace Libs::Graphics::FrameGen
