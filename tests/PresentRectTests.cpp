// ComputePresentRect (presentRect.h): the game's 16:9 image is centred with bars in a window of another shape,
// fills a window within 9.5 % of 16:9, and always fills with stretch on. PresentMapX/Y map the mouse into the image.

#include "graphics/presentation/window/presentRect.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace {

int g_failures = 0;

void Expect(const char* what, uint32_t ww, uint32_t wh, uint32_t sw, uint32_t sh, bool stretch, uint32_t x, uint32_t y,
            uint32_t w, uint32_t h) {
	const auto r = Libs::Graphics::ComputePresentRect(ww, wh, sw, sh, stretch);
	if (r.x != x || r.y != y || r.width != w || r.height != h) {
		std::printf("PresentRectTests: FAIL %s: got %ux%u+%u+%u want %ux%u+%u+%u\n", what, r.width, r.height, r.x, r.y,
		            w, h, x, y);
		g_failures++;
	}
}

void ExpectNear(const char* what, float got, float want) {
	if (std::fabs(got - want) > 1e-4f) {
		std::printf("PresentRectTests: FAIL %s: got %f want %f\n", what, got, want);
		g_failures++;
	}
}

} // namespace

int main() {
	using namespace Libs::Graphics;
	// 16:9 windows fill.
	Expect("1440p", 2560, 1440, 3840, 2160, false, 0, 0, 2560, 1440);
	Expect("4K", 3840, 2160, 3840, 2160, false, 0, 0, 3840, 2160);
	// A 1440p window whose client area lost the title bar and taskbar (5-9 % wider than 16:9): fills.
	Expect("1440p shrunk", 2560, 1369, 3840, 2160, false, 0, 0, 2560, 1369);
	Expect("1440p shrunk more", 2560, 1334, 3840, 2160, false, 0, 0, 2560, 1334);
	Expect("1440p at 150 %", 2560, 1323, 3840, 2160, false, 0, 0, 2560, 1323);
	Expect("1080p shrunk", 1920, 1001, 3840, 2160, false, 0, 0, 1920, 1001);
	// 21:9 ultrawide: bars left and right, 3440x1440 -> 2560x1440 at x=440.
	Expect("3440x1440", 3440, 1440, 3840, 2160, false, 440, 0, 2560, 1440);
	Expect("2560x1080", 2560, 1080, 3840, 2160, false, 320, 0, 1920, 1080);
	// 32:9 super ultrawide.
	Expect("5120x1440", 5120, 1440, 3840, 2160, false, 1280, 0, 2560, 1440);
	// 4:3 and 16:10: bars top and bottom (16:10 is 10 % off, past the 9.5 % tolerance, so it gets bars).
	Expect("1440x1080", 1440, 1080, 3840, 2160, false, 0, 135, 1440, 810);
	Expect("1920x1200", 1920, 1200, 3840, 2160, false, 0, 60, 1920, 1080);
	// Stretch on: always the whole window.
	Expect("stretch 21:9", 3440, 1440, 3840, 2160, true, 0, 0, 3440, 1440);
	// Degenerate sizes: the whole window.
	Expect("no source", 2560, 1440, 0, 0, false, 0, 0, 2560, 1440);
	Expect("no window", 0, 0, 3840, 2160, false, 0, 0, 0, 0);

	// Mouse mapping through the 3440x1440 box: the left bar maps to 0, the box centre to 0.5, the right bar to 1.
	PublishPresentRect(ComputePresentRect(3440, 1440, 3840, 2160, false), 3440, 1440);
	ExpectNear("map left bar", PresentMapX(100.0f / 3440.0f), 0.0f);
	ExpectNear("map centre", PresentMapX(0.5f), 0.5f);
	ExpectNear("map box left edge", PresentMapX(440.0f / 3440.0f), 0.0f);
	ExpectNear("map right bar", PresentMapX(3400.0f / 3440.0f), 1.0f);
	ExpectNear("map y unchanged", PresentMapY(0.25f), 0.25f);
	// Back to a filling window: identity.
	PublishPresentRect(ComputePresentRect(2560, 1440, 3840, 2160, false), 2560, 1440);
	ExpectNear("identity x", PresentMapX(0.3f), 0.3f);

	std::printf("PresentRectTests: %s (%d failures)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
	return g_failures == 0 ? 0 : 1;
}
