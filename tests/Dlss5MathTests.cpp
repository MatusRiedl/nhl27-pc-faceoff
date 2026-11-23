// dlss5_math_tests: the Vulkan-free decisions of the DLSS 5 pass (dlss5Math.h).

#include "graphics/presentation/fsr/dlss5Math.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace {

using namespace Libs::Graphics::Dlss5Math;

int g_failures = 0;

#define CHECK(condition)                                                                   \
	do {                                                                                   \
		if (!(condition)) {                                                                \
			std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
			g_failures++;                                                                  \
		}                                                                                  \
	} while (false)

bool Close(float a, float b) {
	return std::abs(a - b) <= 1e-5f * std::max(1.0f, std::max(std::abs(a), std::abs(b)));
}

void TestMotionScale() {
	// 1280x720 render, a 1920x1080 target: SR's render-pixel scale grows by 1.5.
	CHECK(Close(MotionScale(1280.0f, 1280, 1920), 1920.0f));
	CHECK(Close(MotionScale(-720.0f, 720, 1080), -1080.0f));
	CHECK(Close(MotionScale(1.0f, 1280, 1920), 1.5f));
	// Native (DLAA): unchanged.
	CHECK(Close(MotionScale(3840.0f, 3840, 3840), 3840.0f));
	// NHL 27: SR 1760x990 render (3520x1980 output), the pass at the 3840x2160 picture.
	CHECK(Close(MotionScale(1760.0f, 1760, 3840), 3840.0f));
	CHECK(Close(MotionScale(1.0f, 990, 2160), 2160.0f / 990.0f));
	// A render size larger than the target (a 1440p window): the scale shrinks.
	CHECK(Close(MotionScale(1.0f, 3840, 2560), 2560.0f / 3840.0f));
	// Nothing usable: 0, never a division by zero.
	CHECK(MotionScale(1.0f, 0, 1920) == 0.0f);
	CHECK(MotionScale(1.0f, 1280, 0) == 0.0f);
	CHECK(MotionScale(std::numeric_limits<float>::quiet_NaN(), 1280, 1920) == 0.0f);
	CHECK(MotionScale(std::numeric_limits<float>::infinity(), 1280, 1920) == 0.0f);
}

void TestTargetSize() {
	// The game's 4K in a 1440p window: the box (the window blit is then 1:1).
	auto t = TargetSize(3840, 2160, 2560, 1440);
	CHECK(t.width == 2560 && t.height == 1440);
	// Full screen at 4K: the picture's size.
	t = TargetSize(3840, 2160, 3840, 2160);
	CHECK(t.width == 3840 && t.height == 2160);
	// A window bigger than the picture: the picture's size, the window blit scales up.
	t = TargetSize(2560, 1440, 3840, 2160);
	CHECK(t.width == 2560 && t.height == 1440);
	// Bigger one way only (stretch to an odd window): the picture's size.
	t = TargetSize(3840, 2160, 3440, 2400);
	CHECK(t.width == 3840 && t.height == 2160);
	// Black bars in a wide window: the 16:9 box.
	t = TargetSize(3840, 2160, 2400, 1350);
	CHECK(t.width == 2400 && t.height == 1350);
	// No box (minimised): the picture's size. No picture: nothing.
	t = TargetSize(3840, 2160, 0, 0);
	CHECK(t.width == 3840 && t.height == 2160);
	t = TargetSize(0, 2160, 2560, 1440);
	CHECK(t.width == 0 && t.height == 0);
}

void TestIntensity() {
	CHECK(ParseIntensity(nullptr) == 1.0f);
	CHECK(ParseIntensity("") == 1.0f);
	CHECK(Close(ParseIntensity("0.5"), 0.5f));
	CHECK(ParseIntensity("0") == 0.0f);
	CHECK(ParseIntensity("1") == 1.0f);
	CHECK(ParseIntensity("2") == 1.0f);
	CHECK(ParseIntensity("-1") == 0.0f);
	CHECK(ParseIntensity("abc") == 1.0f);
	CHECK(ParseIntensity("0.5x") == 1.0f);
	CHECK(ParseIntensity("nan") == 1.0f);
}

void TestAbsolutePath() {
	CHECK(IsAbsoluteWindowsPath(L"C:\\Faceoff\\addons\\dlss5\\nvngx_dlssnr.dll"));
	CHECK(IsAbsoluteWindowsPath(L"d:/games/nvngx_dlssnr.dll"));
	CHECK(IsAbsoluteWindowsPath(L"\\\\server\\share\\nvngx_dlssnr.dll"));
	CHECK(!IsAbsoluteWindowsPath(nullptr));
	CHECK(!IsAbsoluteWindowsPath(L""));
	CHECK(!IsAbsoluteWindowsPath(L"nvngx_dlssnr.dll"));
	CHECK(!IsAbsoluteWindowsPath(L"addons\\dlss5\\nvngx_dlssnr.dll"));
	CHECK(!IsAbsoluteWindowsPath(L"C:nvngx_dlssnr.dll"));
	CHECK(!IsAbsoluteWindowsPath(L"\\nvngx_dlssnr.dll"));
	CHECK(!IsAbsoluteWindowsPath(L"\\\\"));
}

void TestInputsFresh() {
	CHECK(InputsFresh(0));
	CHECK(InputsFresh(kMaxPresentsWithoutInputs));
	CHECK(!InputsFresh(kMaxPresentsWithoutInputs + 1));
}

} // namespace

int main() {
	TestMotionScale();
	TestTargetSize();
	TestIntensity();
	TestAbsolutePath();
	TestInputsFresh();
	if (g_failures != 0) {
		std::fprintf(stderr, "dlss5_math_tests: %d check(s) failed\n", g_failures);
		return 1;
	}
	std::printf("dlss5_math_tests: all checks passed\n");
	return 0;
}
