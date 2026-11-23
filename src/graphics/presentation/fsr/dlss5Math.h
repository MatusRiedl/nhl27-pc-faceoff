#pragma once

// DLSS 5 (KYTY_DLSS5, dlss5Pass.h): the small decisions, free of Vulkan and Windows so
// dlss5_math_tests can check them.

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cwchar>

namespace Libs::Graphics::Dlss5Math {

// The model takes depth and motion at its own size (TargetSize), in pixels of that size. Super
// Resolution's vectors are scaled to render pixels (the SR scale); the pass blits them unchanged
// from render size to its size, so the scale grows by target / render (per axis).
[[nodiscard]] inline float MotionScale(float sr_scale, uint32_t render, uint32_t target) {
	if (render == 0 || target == 0 || !std::isfinite(sr_scale)) return 0.0f;
	return sr_scale * static_cast<float>(target) / static_cast<float>(render);
}

// The size the model works at, chosen once on the first picture and kept for the session (its
// feature is never rebuilt). NHL 27 presents a fixed 3840x2160 picture whatever Super Resolution's
// output size is (that changes with the scene), so the size comes from the present side, not from SR:
// the image's box in the window when the picture is at least that big both ways (the game's 4K in a
// smaller window: the copy into the model's input does the downscale and the window blit is 1:1),
// else the picture's own size (the window blit then scales up, as it does without DLSS 5).
struct Extent {
	uint32_t width  = 0;
	uint32_t height = 0;
};
[[nodiscard]] inline Extent TargetSize(uint32_t picture_width, uint32_t picture_height, uint32_t box_width,
                                       uint32_t box_height) {
	if (picture_width == 0 || picture_height == 0) return {};
	if (box_width != 0 && box_height != 0 && box_width <= picture_width && box_height <= picture_height) {
		return {box_width, box_height};
	}
	return {picture_width, picture_height};
}

// KYTY_DLSS5_INTENSITY: 0..1, default 1. Anything unreadable is the default.
[[nodiscard]] inline float ParseIntensity(const char* value) {
	if (value == nullptr || value[0] == '\0') return 1.0f;
	char*       end    = nullptr;
	const float parsed = std::strtof(value, &end);
	if (end == value || *end != '\0' || !std::isfinite(parsed)) return 1.0f;
	return parsed < 0.0f ? 0.0f : parsed > 1.0f ? 1.0f : parsed;
}

// KYTY_DLSS5_MODEL_PATH must be absolute: a drive path (C:\...) or a UNC path (\\server\...).
[[nodiscard]] inline bool IsAbsoluteWindowsPath(const wchar_t* path) {
	if (path == nullptr) return false;
	const auto letter = [](wchar_t c) { return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z'); };
	const auto slash  = [](wchar_t c) { return c == L'\\' || c == L'/'; };
	if (letter(path[0]) && path[1] == L':' && slash(path[2])) return true;
	return slash(path[0]) && slash(path[1]) && path[2] != L'\0';
}

// Inputs come from Super Resolution's frames. A present that has seen no new inputs for
// more than this many presents is not a Super Resolution frame (menus, a backend fallback),
// and the model is skipped for it.
inline constexpr uint32_t kMaxPresentsWithoutInputs = 3;

[[nodiscard]] inline bool InputsFresh(uint32_t presents_since_new_inputs) {
	return presents_since_new_inputs <= kMaxPresentsWithoutInputs;
}

} // namespace Libs::Graphics::Dlss5Math
