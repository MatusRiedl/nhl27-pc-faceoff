// Tests of the DLSS frame generation decisions and camera maths (src/graphics/presentation/fsr/
// frameGenMath.h, frameGenPolicy.h; v0.30.0 KYTY_DLSSG). No GPU, no Streamline, no emulator.
#include "graphics/presentation/fsr/frameGenMath.h"
#include "graphics/presentation/fsr/frameGenPolicy.h"
#include "graphics/presentation/fsr/nhl27Fsr3Constants.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numbers>
#include <vector>

namespace {

using namespace Libs::Graphics::FrameGen;

int g_failures = 0;

#define CHECK(condition)                                                                   \
	do {                                                                                   \
		if (!(condition)) {                                                                \
			std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
			g_failures++;                                                                  \
		}                                                                                  \
	} while (false)

bool Close(float a, float b, float tolerance = 1e-4f) {
	return std::abs(a - b) <= tolerance * std::max(1.0f, std::max(std::abs(a), std::abs(b)));
}

bool CloseMat(const Mat4& a, const Mat4& b, float tolerance = 1e-4f) {
	for (size_t i = 0; i < 16; i++) {
		if (!Close(a.m[i], b.m[i], tolerance)) {
			return false;
		}
	}
	return true;
}

// The camera of the tests: 0.1 .. 2000, 50 degrees vertical, 1920x1080.
constexpr float TestNear = 0.1f;
constexpr float TestFar  = 2000.0f;
const float     TestTanHalfFov = std::tan(50.0f * std::numbers::pi_v<float> / 360.0f);

float ZScale(float n, float f) { return -n / (f - n); }
float ZOffset(float n, float f) { return n * f / (f - n); }

Projection TestProjection() {
	return ProjectionFromConstants(ZScale(TestNear, TestFar), ZOffset(TestNear, TestFar), TestTanHalfFov, 1920, 1080);
}

// A world-to-view matrix for a camera at position with yaw (radians, about +y).
Mat4 ViewFor(float x, float y, float z, float yaw) {
	Mat4 view_to_world = Mat4::Identity();
	view_to_world.At(0, 0) = std::cos(yaw);
	view_to_world.At(0, 2) = -std::sin(yaw);
	view_to_world.At(2, 0) = std::sin(yaw);
	view_to_world.At(2, 2) = std::cos(yaw);
	view_to_world.At(3, 0) = x;
	view_to_world.At(3, 1) = y;
	view_to_world.At(3, 2) = z;
	return *Inverse(view_to_world);
}

Vec4 Ndc(const Vec4& clip) { return {clip.x / clip.w, clip.y / clip.w, clip.z / clip.w, 1.0f}; }

void TestProjectionFromConstants() {
	Libs::Graphics::Nhl27Fsr3Constants c;
	c.words[0] = std::bit_cast<uint32_t>(ZScale(TestNear, TestFar));
	c.words[1] = std::bit_cast<uint32_t>(ZOffset(TestNear, TestFar));
	c.words[3] = std::bit_cast<uint32_t>(TestTanHalfFov);
	CHECK(Close(c.Near(), TestNear, 1e-3f));
	CHECK(Close(c.Far(), TestFar, 1e-3f));

	const auto p = TestProjection();
	CHECK(p.Valid());
	CHECK(Close(p.Near(), c.Near(), 1e-5f));
	CHECK(Close(p.Far(), c.Far(), 1e-5f));
	CHECK(Close(p.FovY(), c.FovY(), 1e-5f));
	CHECK(Close(p.AspectRatio(), 1920.0f / 1080.0f, 1e-5f));

	const auto view_to_clip = ViewToClip(p);
	// Reversed depth: 1 at the near plane, 0 at the far plane, the constants' formula in between.
	CHECK(Close(Ndc(Transform({0, 0, TestNear, 1}, view_to_clip)).z, 1.0f, 1e-4f));
	CHECK(Close(Ndc(Transform({0, 0, TestFar, 1}, view_to_clip)).z, 0.0f, 1e-4f));
	const float mid = 10.0f;
	CHECK(Close(Ndc(Transform({0, 0, mid, 1}, view_to_clip)).z, c.Float(0) + c.Float(1) / mid, 1e-5f));
	// The edges of the field of view.
	CHECK(Close(Ndc(Transform({0, TestTanHalfFov * mid, mid, 1}, view_to_clip)).y, 1.0f, 1e-4f));
	CHECK(Close(Ndc(Transform({TestTanHalfFov * mid * 1920.0f / 1080.0f, 0, mid, 1}, view_to_clip)).x, 1.0f, 1e-4f));
	// Jitter moves the centre by its clip-space amount.
	const auto jittered = Ndc(Transform({0, 0, mid, 1}, ViewToClip(p, 0.002f, -0.001f)));
	CHECK(Close(jittered.x, 0.002f, 1e-4f) && Close(jittered.y, -0.001f, 1e-4f));
	// An invalid block is refused.
	auto bad   = p;
	bad.z_scale = 0.5f;
	CHECK(!bad.Valid());
}

void TestInverse() {
	const auto view_to_clip = ViewToClip(TestProjection(), 0.001f, 0.002f);
	const auto inverse      = Inverse(view_to_clip);
	CHECK(inverse.has_value());
	if (inverse) {
		CHECK(CloseMat(Mul(view_to_clip, *inverse), Mat4::Identity(), 1e-4f));
	}
	const auto view = ViewFor(3, 1.5f, -7, 0.6f);
	const auto back = Inverse(view);
	CHECK(back.has_value() && CloseMat(Mul(*back, view), Mat4::Identity(), 1e-5f));
	Mat4 singular = Mat4::Identity();
	singular.At(2, 2) = 0;
	CHECK(!Inverse(singular).has_value());
	Mat4 not_finite = Mat4::Identity();
	not_finite.At(1, 2) = std::nanf("");
	CHECK(!Inverse(not_finite).has_value());
	CHECK(!Inverse(Mat4 {}).has_value());
	CHECK(CloseMat(Transpose(Transpose(view)), view, 0));
}

void TestClipToPrevClip() {
	const auto p            = TestProjection();
	const auto view_to_clip = ViewToClip(p);
	// No camera found: identity views, the same projection -> identity.
	const auto still = ClipToPrevClip(view_to_clip, Mat4::Identity(), view_to_clip, Mat4::Identity());
	CHECK(still.has_value() && CloseMat(*still, Mat4::Identity(), 1e-4f));

	// A camera that moved and turned: a world point lands where the previous camera saw it.
	const auto prev_view = ViewFor(0, 1.8f, -20, 0.0f);
	const auto view      = ViewFor(1.0f, 1.8f, -19, 0.05f);
	const auto transform = ClipToPrevClip(view_to_clip, view, view_to_clip, prev_view);
	CHECK(transform.has_value());
	if (transform) {
		for (const Vec4 world : {Vec4 {0, 1, 0, 1}, Vec4 {4, 2, 10, 1}, Vec4 {-3, 0.5f, -5, 1}}) {
			const auto clip      = Transform(Transform(world, view), view_to_clip);
			const auto prev_clip = Transform(Transform(world, prev_view), view_to_clip);
			const auto mapped    = Ndc(Transform(clip, *transform));
			const auto expected  = Ndc(prev_clip);
			CHECK(Close(mapped.x, expected.x, 1e-3f) && Close(mapped.y, expected.y, 1e-3f) &&
			      Close(mapped.z, expected.z, 1e-3f));
		}
	}
	// The camera basis comes back from the world-to-view matrix.
	const auto basis = CameraFromView(ViewFor(5, 2, -3, 0.0f));
	CHECK(basis.has_value());
	if (basis) {
		CHECK(Close(basis->position[0], 5) && Close(basis->position[1], 2) && Close(basis->position[2], -3));
		CHECK(Close(basis->forward[2], 1) && Close(basis->right[0], 1) && Close(basis->up[1], 1));
	}
}

void TestRigidView() {
	CHECK(IsRigidView(ViewFor(3, 1, -2, 0.4f)));
	CHECK(!IsRigidView(Mat4::Identity()));
	auto scaled = ViewFor(3, 1, -2, 0.4f);
	for (int i = 0; i < 3; i++) {
		scaled.At(i, i) *= 2.0f;
	}
	CHECK(!IsRigidView(scaled));
	CHECK(!IsRigidView(ViewToClip(TestProjection())));
	auto far_away = ViewFor(3, 1, -2, 0.4f);
	far_away.At(3, 0) = 1e7f;
	CHECK(!IsRigidView(far_away));
}

// Writes a matrix into the words at a byte offset, row by row or column by column.
void Plant(std::vector<uint32_t>& words, size_t offset, const Mat4& a, bool transposed) {
	for (int row = 0; row < 4; row++) {
		for (int col = 0; col < 4; col++) {
			const size_t index = offset / 4 + static_cast<size_t>(transposed ? col * 4 + row : row * 4 + col);
			words[index]       = std::bit_cast<uint32_t>(a.At(row, col));
		}
	}
}

// 576 bytes of decoys: a counter's worth of plausible floats (colours, positions, time).
std::vector<uint32_t> DecoyBuffer() {
	std::vector<uint32_t> words(144);
	uint32_t              state = 12345;
	for (auto& word : words) {
		state = state * 1664525u + 1013904223u;
		word  = std::bit_cast<uint32_t>(static_cast<float>(state >> 8) / 16777216.0f * 4.0f - 2.0f);
	}
	return words;
}

void TestCameraScan() {
	const auto expected = TestProjection();
	{
		// Decoys only: an identity matrix and a projection of another field of view among noise.
		auto words = DecoyBuffer();
		Plant(words, 0, Mat4::Identity(), false);
		auto other_fov = expected;
		other_fov.y_scale *= 1.3f;
		other_fov.x_scale *= 1.3f;
		Plant(words, 64, ViewToClip(other_fov), false);
		CHECK(!ScanForCamera(words, expected).has_value());
	}
	{
		// The game's layout guess: jittered, y flipped, row by row at 128, the view column by column at 192.
		auto words = DecoyBuffer();
		Plant(words, 0, Mat4::Identity(), false);
		auto projection = ViewToClip(expected, 0.0021f, -0.0013f);
		projection.At(1, 1) = -projection.At(1, 1);
		Plant(words, 128, projection, false);
		Plant(words, 192, ViewFor(12, 3, -40, 1.1f), true);
		const auto match = ScanForCamera(words, expected);
		CHECK(match.has_value());
		if (match) {
			CHECK(match->projection_offset == 128 && !match->projection_transposed);
			CHECK(match->y_flipped && !match->looks_down_minus_z);
			CHECK(match->view_offset == 192 && match->view_transposed);
			CHECK(match->score == CameraScoreUsable + CameraScoreView);
		}
	}
	{
		// Column by column and right-handed (w = -z), no view anywhere near.
		auto words      = DecoyBuffer();
		auto projection = ViewToClip(expected);
		projection.At(2, 2) = -projection.At(2, 2);
		projection.At(2, 3) = -1.0f;
		Plant(words, 320, projection, true);
		const auto match = ScanForCamera(words, expected);
		CHECK(match.has_value());
		if (match) {
			CHECK(match->projection_offset == 320 && match->projection_transposed && match->looks_down_minus_z);
			CHECK(!match->view_offset.has_value() && match->score == CameraScoreUsable);
		}
	}
	{
		// The shape matches but the depth terms do not (another near plane, an infinite far plane):
		// found, not usable.
		auto words    = DecoyBuffer();
		auto infinite = expected;
		infinite.z_scale  = -1e-7f;
		infinite.z_offset = 0.5f;
		Plant(words, 64, ViewToClip(infinite), false);
		const auto match = ScanForCamera(words, expected);
		CHECK(match.has_value() && match->score == CameraScoreShape && match->score < CameraScoreUsable);
	}
	{
		// Too short a buffer and an invalid expectation find nothing.
		std::vector<uint32_t> short_words(12, 0);
		CHECK(!ScanForCamera(short_words, expected).has_value());
		auto words = DecoyBuffer();
		Plant(words, 128, ViewToClip(expected), false);
		CHECK(!ScanForCamera(words, Projection {}).has_value());
	}
}

// The game's projection for the scanner's flags: ViewToClip's with row 1 / row 2 negated.
Mat4 GameProjection(const Projection& p, bool y_flipped, bool minus_z) {
	auto r = ViewToClip(p);
	for (int col = 0; col < 4; col++) {
		if (y_flipped) {
			r.At(1, col) = -r.At(1, col);
		}
		if (minus_z) {
			r.At(2, col) = -r.At(2, col);
		}
	}
	return r;
}

void TestViewFromGame() {
	const auto expected = TestProjection();
	for (const bool y_flipped : {false, true}) {
		for (const bool minus_z : {false, true}) {
			// A game camera in the game's own convention, stored beside its projection.
			const auto game_view       = ViewFromGame(ViewFor(4, 2, -9, 0.7f), y_flipped, minus_z);
			const auto game_projection = GameProjection(expected, y_flipped, minus_z);
			auto       words           = DecoyBuffer();
			Plant(words, 64, game_projection, true);
			Plant(words, 128, game_view, false);
			const auto match = ScanForCamera(words, expected);
			CHECK(match.has_value());
			if (!match) {
				continue;
			}
			CHECK(match->y_flipped == y_flipped && match->looks_down_minus_z == minus_z);
			CHECK(match->view_offset == 128 && !match->view_transposed);
			// Converted, the view with ViewToClip gives the game's own clip positions.
			const auto ours = ViewFromGame(game_view, match->y_flipped, match->looks_down_minus_z);
			CHECK(IsRigidView(ours));
			CHECK(CloseMat(Mul(ours, ViewToClip(expected)), Mul(game_view, game_projection), 1e-4f));
		}
	}
}

void TestCameraTracker() {
	const auto         expected = TestProjection();
	constexpr uint64_t Shader = 0x1234abcd5678ef00ull, Other = 0x42;
	constexpr uint32_t Slot   = 2;
	const auto         game_projection = GameProjection(expected, true, false);
	const auto         buffer_with     = [&](const Mat4& game_view) {
        auto words = DecoyBuffer();
        Plant(words, 128, game_projection, false);
        Plant(words, 192, game_view, true);
        return words;
	};
	const auto view_a = ViewFromGame(ViewFor(12, 3, -40, 1.1f), true, false);
	const auto view_b = ViewFromGame(ViewFor(13, 3, -38, 1.2f), true, false);
	const auto size   = static_cast<uint64_t>(DecoyBuffer().size() * 4);
	{
		// Locks on the third scanned frame, then reads once per frame; an absent frame keeps the view.
		CameraTracker tracker;
		CHECK(!tracker.Wants(0, Shader, Slot, size)); // no expected projection yet
		tracker.SetExpected(expected);
		const auto words = buffer_with(view_a);
		CHECK(tracker.Wants(0, Shader, Slot, size));
		CHECK(tracker.Feed(0, Shader, Slot, size, words) == CameraTracker::Event::Candidate);
		CHECK(!tracker.Wants(1, Shader, Slot, size)); // not a scan frame
		CHECK(tracker.Wants(15, Shader, Slot, size));
		CHECK(tracker.Feed(15, Shader, Slot, size, words) == CameraTracker::Event::None);
		CHECK(!tracker.Lock().has_value());
		CHECK(tracker.Wants(30, Shader, Slot, size));
		CHECK(tracker.Feed(30, Shader, Slot, size, words) == CameraTracker::Event::Locked);
		CHECK(tracker.Lock().has_value() && tracker.Lock()->shader == Shader && tracker.Lock()->slot == Slot);
		CHECK(CloseMat(Mul(tracker.View(), ViewToClip(expected)), Mul(view_a, game_projection), 1e-4f));
		CHECK(!tracker.Wants(30, Shader, Slot, size)); // read this frame already
		CHECK(!tracker.Wants(31, Other, Slot, size) && !tracker.Wants(31, Shader, Slot + 1, size));
		CHECK(!tracker.Wants(31, Shader, Slot, 128)); // too small to hold the view
		CHECK(tracker.Wants(31, Shader, Slot, size));
		tracker.Feed(31, Shader, Slot, size, buffer_with(view_b));
		CHECK(CloseMat(Mul(tracker.View(), ViewToClip(expected)), Mul(view_b, game_projection), 1e-4f));
		CHECK(!tracker.Wants(31, Shader, Slot, size));
		// Frame 32 does not draw the shader; frame 33 holds no rigid view: the last good one stays.
		auto broken = buffer_with(view_b);
		Plant(broken, 192, Mat4::Identity(), true);
		CHECK(tracker.Wants(33, Shader, Slot, size));
		tracker.Feed(33, Shader, Slot, size, broken);
		CHECK(CloseMat(Mul(tracker.View(), ViewToClip(expected)), Mul(view_b, game_projection), 1e-4f));
		CHECK(tracker.Reads() == 1 && tracker.Rejects() == 1);
		CHECK(tracker.Wants(33, Shader, Slot, size)); // a rejected read tries the next draw
	}
	{
		// Two cameras through one shader and slot in one frame (a per-object or second camera): never locks.
		CameraTracker tracker;
		tracker.SetExpected(expected);
		CHECK(tracker.Feed(0, Shader, Slot, size, buffer_with(view_a)) == CameraTracker::Event::Candidate);
		CHECK(tracker.Feed(0, Shader, Slot, size, buffer_with(view_b)) == CameraTracker::Event::Varies);
		for (uint64_t frame = 15; frame <= 150; frame += 15) {
			CHECK(tracker.Feed(frame, Shader, Slot, size, buffer_with(view_a)) != CameraTracker::Event::Locked);
		}
		CHECK(!tracker.Lock().has_value());
		// The same view through two draws of one frame is fine.
		CameraTracker steady;
		steady.SetExpected(expected);
		for (uint64_t frame = 0; frame <= 30; frame += 15) {
			steady.Feed(frame, Shader, Slot, size, buffer_with(view_a));
			steady.Feed(frame, Shader, Slot, size, buffer_with(view_a));
		}
		CHECK(steady.Lock().has_value());
	}
	{
		// Decoys (another field of view, no view near a projection) never become candidates.
		CameraTracker tracker;
		tracker.SetExpected(expected);
		auto other_fov = expected;
		other_fov.x_scale *= 1.3f;
		other_fov.y_scale *= 1.3f;
		auto decoy = buffer_with(view_a);
		Plant(decoy, 128, ViewToClip(other_fov), false);
		auto no_view = DecoyBuffer();
		Plant(no_view, 320, ViewToClip(expected), true);
		for (uint64_t frame = 0; frame <= 300; frame += 15) {
			CHECK(tracker.Feed(frame, Shader, Slot, size, decoy) == CameraTracker::Event::None);
			CHECK(tracker.Feed(frame, Other, Slot, size, no_view) == CameraTracker::Event::None);
		}
		CHECK(!tracker.Lock().has_value());
	}
	{
		// The per-frame budget and the size window.
		CameraTracker tracker;
		tracker.SetExpected(expected);
		uint32_t wanted = 0;
		for (uint32_t i = 0; i < CameraTracker::MaxBuffers + 10; i++) {
			wanted += tracker.Wants(0, Shader, i, size) ? 1 : 0;
		}
		CHECK(wanted == CameraTracker::MaxBuffers);
		CHECK(tracker.Wants(15, Shader, Slot, size)); // a new scan frame has a new budget
		CHECK(!tracker.Wants(15, Shader, Slot, 32) && !tracker.Wants(15, Shader, Slot, CameraTracker::MaxBytes + 16));
	}
}

void TestLoaderDecision() {
	CHECK(DecideLoader({}).loader == Loader::Plain && DecideLoader({}).reason == "off");
	LoaderInputs in {.frame_gen_switch = true};
	CHECK(DecideLoader(in).reason == "not-built");
	in.built_with_dlssg = true;
	CHECK(DecideLoader(in).reason == "needs-KYTY_DLSS=1");
	in.dlss        = true;
	in.missing_dll = "nvngx_dlssg.dll";
	CHECK(DecideLoader(in).loader == Loader::Plain && DecideLoader(in).reason == "dll-missing");
	in.missing_dll = {};
	CHECK(DecideLoader(in).loader == Loader::Streamline && DecideLoader(in).reason == "ok");
	in.frame_gen_switch = false;
	CHECK(DecideLoader(in).loader == Loader::Plain);
	CHECK(RequiredDlls.size() == 6 && RequiredDlls[0] == "sl.interposer.dll");

	CHECK(FramesToGenerate(1, 0) == 1);
	CHECK(FramesToGenerate(3, 0) == 1);
	CHECK(FramesToGenerate(3, 1) == 1);
	CHECK(FramesToGenerate(3, 3) == 3);
	CHECK(FramesToGenerate(5, 4) == 3);
	CHECK(FramesToGenerate(0, 3) == 1);
}

void TestModeGate() {
	ModeGate gate(ModeGate::Config {.on_after = 3, .off_after = 2});
	const ModeGate::Inputs good {.tags_ready = true, .window_ok = true};
	const ModeGate::Inputs menu {.tags_ready = false, .window_ok = true};
	const ModeGate::Inputs small {.tags_ready = true, .window_ok = false};
	CHECK(!gate.Update(good).on);
	CHECK(!gate.Update(good).on);
	auto step = gate.Update(good);
	CHECK(step.on && step.changed);
	CHECK(gate.Update(good).on && !gate.Update(good).changed);
	step = gate.Update(menu);
	CHECK(step.on && !step.changed && step.reason == "no-tags");
	CHECK(gate.Update(good).on); // an eligible frame restarts the count
	CHECK(gate.Update(small).on);
	step = gate.Update(small);
	CHECK(!step.on && step.changed && step.reason == "window");
	// One eligible frame is not enough to turn back on, and an ineligible one restarts the count.
	CHECK(!gate.Update(good).on);
	CHECK(!gate.Update(menu).on);
	CHECK(!gate.Update(good).on && !gate.Update(good).on);
	CHECK(gate.Update(good).changed && gate.On());
	// A failure turns it off for good.
	CHECK(gate.Fail("status:eFailReflexNotDetectedAtRuntime"));
	CHECK(gate.Failed() && !gate.On());
	for (int i = 0; i < 10; i++) {
		step = gate.Update(good);
		CHECK(!step.on && !step.changed);
	}
	CHECK(step.reason == "status:eFailReflexNotDetectedAtRuntime");
	CHECK(!gate.Fail("again"));
	CHECK(gate.Update(good).reason == "status:eFailReflexNotDetectedAtRuntime");
}

void TestHold() {
	ModeGate gate(ModeGate::Config {.on_after = 1, .off_after = 1, .hold = true});
	for (int i = 0; i < 5; i++) {
		const auto step = gate.Update({.tags_ready = true, .window_ok = true});
		CHECK(!step.on && !step.changed && step.reason == "hold");
	}
	CHECK(!gate.On() && !gate.Failed());
}

void TestFrameLedger() {
	FrameLedger ledger;
	CHECK(ledger.TakeForPresent(0).index == 0 && !ledger.TakeForPresent(0).new_frame);
	// Before any flip: indices start at 1. PeekTag commits nothing.
	CHECK(ledger.PeekTag(0) == 1 && ledger.PeekTag(0) == 1 && ledger.LastTagged() == 0);
	CHECK(ledger.Tag(0) == 1);
	CHECK(ledger.PeekTag(0) == 2 && ledger.PeekTag(9) == 9);
	ledger.BindFlip(10);
	auto take = ledger.TakeForPresent(10);
	CHECK(take.index == 1 && take.new_frame);
	// Presenting the same flip again (a repeat) carries the last index, as not new.
	take = ledger.TakeForPresent(10);
	CHECK(take.index == 1 && !take.new_frame);
	// The GPU thread ran a frame ahead of the present: each flip still gets its own frame.
	CHECK(ledger.Tag(0) == 2);
	ledger.BindFlip(11);
	CHECK(ledger.Tag(0) == 3);
	ledger.BindFlip(12);
	take = ledger.TakeForPresent(11);
	CHECK(take.index == 2 && take.new_frame);
	take = ledger.TakeForPresent(12);
	CHECK(take.index == 3 && take.new_frame);
	// A flip without an upscale pass (menu, fade) binds nothing.
	ledger.BindFlip(13);
	take = ledger.TakeForPresent(13);
	CHECK(take.index == 3 && !take.new_frame);
	// A flip that was never presented is dropped when a later one is.
	ledger.Tag(0);
	ledger.BindFlip(14);
	CHECK(ledger.Tag(0) == 5);
	ledger.BindFlip(15);
	take = ledger.TakeForPresent(15);
	CHECK(take.index == 5 && take.new_frame);
	CHECK(!ledger.TakeForPresent(14).new_frame);
	// Two upscale passes before a flip: never the same index, the flip takes the newest.
	CHECK(ledger.Tag(0) == 6);
	CHECK(ledger.Tag(0) == 7);
	ledger.BindFlip(16);
	CHECK(ledger.TakeForPresent(16).index == 7);
	// The flip-submit side's index is used when it is ahead; indices only grow.
	CHECK(ledger.Tag(20) == 20);
	CHECK(ledger.Tag(3) == 21);
	CHECK(ledger.LastTagged() == 21);
	// A present that is not a flip (blank frame, last-frame repeat) is never new.
	ledger.BindFlip(17);
	CHECK(!ledger.TakeForPresent(0).new_frame);
	CHECK(ledger.TakeForPresent(17).new_frame);
	// Request 0 never binds.
	ledger.Tag(0);
	ledger.BindFlip(0);
	CHECK(!ledger.TakeForPresent(0).new_frame);
}

void TestModeSequencer() {
	using State = ModeSequencer::State;
	ModeSequencer seq(3);
	CHECK(!seq.FrameGenPresentMode());
	auto out = seq.BeforePresent(false);
	CHECK(!out.set_options && !out.request_recreate && seq.Current() == State::Off);
	// Turning on: recreate in a frame-generation present mode, then the options.
	out = seq.BeforePresent(true);
	CHECK(out.request_recreate && !out.set_options && seq.FrameGenPresentMode());
	CHECK(!seq.BeforePresent(true).request_recreate); // still waiting for the recreate
	out = seq.Recreated(true);
	CHECK(out.set_options && out.options_on && seq.Current() == State::On);
	CHECK(!seq.BeforePresent(true).set_options);
	// Turning off: options off first, one present, then the recreate in the plain mode.
	out = seq.BeforePresent(false);
	CHECK(out.set_options && !out.options_on && !out.request_recreate && seq.FrameGenPresentMode());
	out = seq.BeforePresent(false);
	CHECK(out.request_recreate && !seq.FrameGenPresentMode());
	CHECK(!seq.Recreated(false).set_options && seq.Current() == State::Off);
	// The gate goes off while the recreate for on is pending: back to the plain mode.
	CHECK(seq.BeforePresent(true).request_recreate);
	out = seq.Recreated(false);
	CHECK(!out.set_options && out.request_recreate && !seq.FrameGenPresentMode());
	seq.Recreated(false);
	CHECK(seq.Current() == State::Off);
	// A resize while on: options off at once, then a cooldown of 3 presents before turning on again.
	seq.BeforePresent(true);
	seq.Recreated(true);
	CHECK(seq.Current() == State::On);
	out = seq.OutsideRecreate();
	CHECK(out.set_options && !out.options_on && seq.Current() == State::Off && !seq.FrameGenPresentMode());
	CHECK(!seq.BeforePresent(true).request_recreate);
	CHECK(!seq.BeforePresent(true).request_recreate);
	CHECK(seq.BeforePresent(true).request_recreate);
	// A resize while off asks for nothing.
	ModeSequencer idle;
	CHECK(!idle.OutsideRecreate().set_options);
}

void TestPresentPlan() {
	CHECK(!PlanPresent(false, true, true, false, false).tag);
	CHECK(!PlanPresent(true, false, true, false, false).tag); // a repeat present: null tags
	CHECK(!PlanPresent(true, true, false, false, false).generate); // no upscale pass this frame
	const auto plain = PlanPresent(true, true, true, false, false);
	CHECK(plain.tag && plain.generate && !plain.reset);
	CHECK(PlanPresent(true, true, true, true, false).reset);
	CHECK(PlanPresent(true, true, true, false, true).reset);

	CHECK(PresentModeFor(false, PresentMode::Fifo, true, true) == PresentMode::Fifo);
	CHECK(PresentModeFor(true, PresentMode::Fifo, true, true) == PresentMode::Mailbox);
	CHECK(PresentModeFor(true, PresentMode::FifoRelaxed, false, true) == PresentMode::Immediate);
	CHECK(PresentModeFor(true, PresentMode::Fifo, false, false) == PresentMode::Fifo);
	CHECK(PresentModeFor(true, PresentMode::Immediate, true, true) == PresentMode::Immediate);
}

} // namespace

int main() {
	TestProjectionFromConstants();
	TestInverse();
	TestClipToPrevClip();
	TestRigidView();
	TestCameraScan();
	TestViewFromGame();
	TestCameraTracker();
	TestLoaderDecision();
	TestModeGate();
	TestHold();
	TestFrameLedger();
	TestModeSequencer();
	TestPresentPlan();
	if (g_failures != 0) {
		std::fprintf(stderr, "frame_gen_tests: %d check(s) failed\n", g_failures);
		return 1;
	}
	std::printf("frame_gen_tests: all checks passed\n");
	return 0;
}
