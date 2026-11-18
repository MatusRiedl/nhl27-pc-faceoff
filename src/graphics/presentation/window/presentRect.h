#ifndef GRAPHICS_PRESENTATION_WINDOW_PRESENTRECT_H_
#define GRAPHICS_PRESENTATION_WINDOW_PRESENTRECT_H_

// Where the game image goes inside the window. The game outputs 16:9; a window of another shape (21:9 ultrawide
// fullscreen, 4:3, 16:10) gets the image centred at its own aspect with black bars, instead of stretched. Within
// kPresentAspectTolerance of the window's shape the image fills it: a "1440p" window whose client area shrank to
// 2560x1334 or 2560x1323 (title bar and taskbar, up to 150 % scaling; 8-9 % wider than 16:9) would otherwise get
// thin bars. 16:10 is 10 % off, so it still gets bars. KYTY_PRESENT_STRETCH=1 always fills.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace Libs::Graphics {

inline constexpr double kPresentAspectTolerance = 0.095;

struct PresentRect {
	uint32_t x      = 0;
	uint32_t y      = 0;
	uint32_t width  = 0;
	uint32_t height = 0;
};

// The rect, in window pixels, for a source of src_w x src_h shown in a window of win_w x win_h.
inline PresentRect ComputePresentRect(uint32_t win_w, uint32_t win_h, uint32_t src_w, uint32_t src_h, bool stretch) {
	const PresentRect full {0, 0, win_w, win_h};
	if (stretch || win_w == 0 || win_h == 0 || src_w == 0 || src_h == 0) {
		return full;
	}
	const double window = static_cast<double>(win_w) / static_cast<double>(win_h);
	const double source = static_cast<double>(src_w) / static_cast<double>(src_h);
	if (std::abs(window / source - 1.0) <= kPresentAspectTolerance) {
		return full;
	}
	if (window > source) {  // wider than the image: bars left and right
		const auto width = std::min(win_w, static_cast<uint32_t>(std::lround(static_cast<double>(win_h) * source)));
		return {(win_w - width) / 2u, 0, width, win_h};
	}
	const auto height = std::min(win_h, static_cast<uint32_t>(std::lround(static_cast<double>(win_w) / source)));
	return {0, (win_h - height) / 2u, win_w, height};
}

inline bool PresentStretchWanted() {
	static const bool stretch = [] {
		const char* value = std::getenv("KYTY_PRESENT_STRETCH");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return stretch;
}

// The last presented rect as fractions of the window, for mapping the mouse (touch pad) into the image.
// Written by the swapchain each frame, read by host input; a torn read is harmless.
inline std::atomic<float> g_present_rect_x {0.0f};
inline std::atomic<float> g_present_rect_y {0.0f};
inline std::atomic<float> g_present_rect_w {1.0f};
inline std::atomic<float> g_present_rect_h {1.0f};

inline void PublishPresentRect(const PresentRect& rect, uint32_t win_w, uint32_t win_h) {
	if (win_w == 0 || win_h == 0) {
		return;
	}
	g_present_rect_x.store(static_cast<float>(rect.x) / static_cast<float>(win_w), std::memory_order_relaxed);
	g_present_rect_y.store(static_cast<float>(rect.y) / static_cast<float>(win_h), std::memory_order_relaxed);
	g_present_rect_w.store(static_cast<float>(rect.width) / static_cast<float>(win_w), std::memory_order_relaxed);
	g_present_rect_h.store(static_cast<float>(rect.height) / static_cast<float>(win_h), std::memory_order_relaxed);
}

// A window position as a fraction (0..1 of the window) mapped into the image, clamped to it.
inline float PresentMapX(float window_fraction) {
	const float w = g_present_rect_w.load(std::memory_order_relaxed);
	return w > 0.0f ? std::clamp((window_fraction - g_present_rect_x.load(std::memory_order_relaxed)) / w, 0.0f, 1.0f)
	                : window_fraction;
}

inline float PresentMapY(float window_fraction) {
	const float h = g_present_rect_h.load(std::memory_order_relaxed);
	return h > 0.0f ? std::clamp((window_fraction - g_present_rect_y.load(std::memory_order_relaxed)) / h, 0.0f, 1.0f)
	                : window_fraction;
}

} // namespace Libs::Graphics

#endif // GRAPHICS_PRESENTATION_WINDOW_PRESENTRECT_H_
