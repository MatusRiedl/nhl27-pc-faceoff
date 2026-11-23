#pragma once

#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>
#include <vector>

namespace Libs::Graphics {
struct GraphicContext;
}

// DLSS 5 ("DLSS-NR", NGX feature 18), experimental: KYTY_DLSS5=1 with KYTY_DLSS=1 in a build with
// KYTY_ENABLE_DLSS, on Windows. Off by default; with the switch off nothing here runs.
//
// The model is the player's own nvngx_dlssnr.dll (never shipped): KYTY_DLSS5_MODEL_PATH (an absolute
// path), else nvngx_dlssnr.dll next to the exe. It refuses calls from any module whose path lacks
// "nvngx.dll", so every call goes through our forwarder, nvngx.dll_dlssnr.dll next to the exe
// (src/dlss5Forwarder). It is loaded in the early DLSS warm-up, before the guest reservation, and
// stays loaded for the process.
//
// Where it runs: on the presented image, HUD included, before the window blit (swapchain.cpp).
// The pass works at one size for the session, chosen on the first picture after Super Resolution's
// first frame (the game's boot pictures are smaller), from the present side
// (Dlss5Math::TargetSize: the image's box in the window, or the picture's size when the window is
// bigger). NHL 27 presents a fixed 3840x2160 picture while Super Resolution's output size changes
// with the scene, so SR's size is never used. The picture is blitted into an RGBA16F copy at that
// size (linear when it shrinks; values stay display-encoded 0..1), evaluated, and the result
// (blitted to the UNORM swapchain, which clamps it to 0..1) replaces the source. Depth and motion
// come from SR: after each successful SR evaluate the bridge blits its render-size depth and
// vectors (nearest) to the pass's size into this pass's own images, nothing until the size is known.
//
// Threads: the inputs are recorded on the thread that runs DlssBridge::Dispatch; the model calls
// (init, create, evaluate) all run on the present thread, as in the one known working integration.
// A mutex guards what the two share. Every image lives until the process exits (no recreate): a
// later change of window size is absorbed by the copy in and the window blit.
//
// Fails closed: any load, init, create or evaluate failure turns DLSS 5 off for the session and
// the frame is presented as before; DLSS Super Resolution goes on. Frame generation (KYTY_DLSSG=1)
// turns it off too. Logs: "NHL27DLSS5 status=... " once per stage, then "NHL27DLSS5 ms=..." every
// 5 s (GPU time of the copy and the evaluate).
namespace Libs::Graphics::Dlss5 {

// KYTY_DLSS5=1 (and KYTY_DLSS=1) in a DLSS build, and nothing has failed.
[[nodiscard]] bool Active();

// DlssWarmUp (vulkanWindow.cpp), before the guest reservation: loads the forwarder and the model.
void LoadEarly();

// Device creation: the model needs VK_NVX_binary_import and VK_NVX_image_view_handle.
void AppendDeviceExtensions(const std::vector<vk::ExtensionProperties>& available,
                            std::vector<const char*>& extensions);

// DlssBridge::Dispatch, only after a successful SR evaluate, in the command buffer that is then
// submitted. depth and motion are the bridge's render-size images, in VK_IMAGE_LAYOUT_GENERAL and
// owned by graphics.queue_family. mv_scale is the scale SR was given (render pixels). display is
// SR's output size, for the log only (the pass's size comes from the present side).
void RecordInputs(GraphicContext& graphics, VkCommandBuffer cmd, VkImage depth, VkImage motion,
                  VkExtent2D render, VkExtent2D display, float mv_scale_x, float mv_scale_y, bool reset);
// After the vkQueueSubmit of that command buffer: publishes the recorded inputs when it worked.
void InputsSubmitted(bool submitted);

struct PresentSource {
	VkImage    image  = VK_NULL_HANDLE; // in VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
	VkFormat   format = VK_FORMAT_UNDEFINED;
	VkExtent2D extent {};
	uint64_t   serial = 0; // changes with every new picture in the frame
	bool       hdr    = false;
	bool       blank  = false;
	VkFormat   target_format = VK_FORMAT_UNDEFINED; // the swapchain's
	VkExtent2D box {}; // the image's box in the window (presentRect.h)
};
struct PresentResult {
	VkImage    image = VK_NULL_HANDLE; // in VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, at the pass's size
	VkExtent2D extent {};              // the pass's size: blit from this, not from the source's size
};
// The present thread (under the presenter's mutex): the model on this picture. A null image means
// present the source as before.
[[nodiscard]] PresentResult RecordPresent(GraphicContext& graphics, VkCommandBuffer cmd,
                                          const PresentSource& source);

} // namespace Libs::Graphics::Dlss5
