#pragma once

// DLSS frame generation (KYTY_DLSSG, v0.30.0, experimental): the camera maths, free of Vulkan and
// Streamline so frame_gen_tests can check it. Matrices are stored row by row and used with row
// vectors (clip = view_position * view_to_clip, translation in the last row); frameGen.cpp converts
// them to Streamline's layout.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace Libs::Graphics::FrameGen {

struct Vec4 {
	float x = 0, y = 0, z = 0, w = 0;
};

struct Mat4 {
	std::array<float, 16> m {};

	[[nodiscard]] constexpr float  At(int row, int col) const { return m[static_cast<size_t>(row * 4 + col)]; }
	[[nodiscard]] constexpr float& At(int row, int col) { return m[static_cast<size_t>(row * 4 + col)]; }

	[[nodiscard]] static constexpr Mat4 Identity() {
		Mat4 r;
		r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
		return r;
	}
	// 16 words as a shader stores them: row by row, or column by column (transposed).
	[[nodiscard]] static Mat4 FromWords(std::span<const uint32_t, 16> words, bool transposed) {
		Mat4 r;
		for (int row = 0; row < 4; row++) {
			for (int col = 0; col < 4; col++) {
				const auto index = static_cast<size_t>(transposed ? col * 4 + row : row * 4 + col);
				r.At(row, col)   = std::bit_cast<float>(words[index]);
			}
		}
		return r;
	}
	[[nodiscard]] bool Finite() const {
		return std::ranges::all_of(m, [](float value) { return std::isfinite(value); });
	}
};

[[nodiscard]] inline Mat4 Mul(const Mat4& a, const Mat4& b) {
	Mat4 r;
	for (int row = 0; row < 4; row++) {
		for (int col = 0; col < 4; col++) {
			float sum = 0;
			for (int k = 0; k < 4; k++) {
				sum += a.At(row, k) * b.At(k, col);
			}
			r.At(row, col) = sum;
		}
	}
	return r;
}

[[nodiscard]] inline Mat4 Transpose(const Mat4& a) {
	Mat4 r;
	for (int row = 0; row < 4; row++) {
		for (int col = 0; col < 4; col++) {
			r.At(row, col) = a.At(col, row);
		}
	}
	return r;
}

// Row vector times matrix.
[[nodiscard]] inline Vec4 Transform(const Vec4& v, const Mat4& a) {
	const float in[4] = {v.x, v.y, v.z, v.w};
	float       out[4] {};
	for (int col = 0; col < 4; col++) {
		for (int k = 0; k < 4; k++) {
			out[col] += in[k] * a.At(k, col);
		}
	}
	return {out[0], out[1], out[2], out[3]};
}

// Gauss-Jordan with partial pivoting (in double); nullopt for a singular or non-finite matrix.
[[nodiscard]] inline std::optional<Mat4> Inverse(const Mat4& a) {
	if (!a.Finite()) {
		return std::nullopt;
	}
	double work[4][8] {};
	double scale = 0;
	for (int row = 0; row < 4; row++) {
		for (int col = 0; col < 4; col++) {
			work[row][col] = a.At(row, col);
			scale          = std::max(scale, std::abs(work[row][col]));
		}
		work[row][4 + row] = 1.0;
	}
	if (scale == 0) {
		return std::nullopt;
	}
	for (int col = 0; col < 4; col++) {
		int pivot = col;
		for (int row = col + 1; row < 4; row++) {
			if (std::abs(work[row][col]) > std::abs(work[pivot][col])) {
				pivot = row;
			}
		}
		if (std::abs(work[pivot][col]) <= 1e-9 * scale) {
			return std::nullopt;
		}
		if (pivot != col) {
			for (int k = 0; k < 8; k++) {
				std::swap(work[pivot][k], work[col][k]);
			}
		}
		const double divisor = work[col][col];
		for (int k = 0; k < 8; k++) {
			work[col][k] /= divisor;
		}
		for (int row = 0; row < 4; row++) {
			if (row == col) {
				continue;
			}
			const double factor = work[row][col];
			for (int k = 0; k < 8; k++) {
				work[row][k] -= factor * work[col][k];
			}
		}
	}
	Mat4 r;
	for (int row = 0; row < 4; row++) {
		for (int col = 0; col < 4; col++) {
			r.At(row, col) = static_cast<float>(work[row][4 + col]);
		}
	}
	return r.Finite() ? std::optional<Mat4>(r) : std::nullopt;
}

// The game's projection as the upscale pass's constants give it (Nhl27Fsr3Constants): reversed
// depth (1 at the near plane, 0 at the far plane), the camera looking down +z. z_scale / z_offset
// are words 0 / 1, so device depth = z_scale + z_offset / view_z. Whether the game's own matrix
// flips y, or looks down -z, is not known: the camera scanner accepts either.
struct Projection {
	float x_scale  = 1;
	float y_scale  = 1;
	float z_scale  = 0;
	float z_offset = 0;

	[[nodiscard]] float Near() const { return z_offset / (1.0f - z_scale); }
	[[nodiscard]] float Far() const { return z_offset / -z_scale; }
	[[nodiscard]] float AspectRatio() const { return y_scale / x_scale; }
	[[nodiscard]] float FovY() const { return 2.0f * std::atan(1.0f / y_scale); }
	[[nodiscard]] bool  Valid() const {
		return std::isfinite(x_scale) && std::isfinite(y_scale) && std::isfinite(z_scale) &&
		       std::isfinite(z_offset) && x_scale > 0 && y_scale > 0 && z_scale < 0 && z_offset > 0 &&
		       Near() > 0 && Far() > Near();
	}
};

// words 0, 1, 3 of the constants (tan(fovY / 2) is word 3) and the render size.
[[nodiscard]] inline Projection ProjectionFromConstants(float z_scale, float z_offset, float tan_half_fov_y,
                                                        uint32_t width, uint32_t height) {
	Projection p;
	p.y_scale  = 1.0f / tan_half_fov_y;
	p.x_scale  = p.y_scale * static_cast<float>(height) / static_cast<float>(width);
	p.z_scale  = z_scale;
	p.z_offset = z_offset;
	return p;
}

// jitter in clip units (2 * pixels / size); 0 for the unjittered matrix.
[[nodiscard]] inline Mat4 ViewToClip(const Projection& p, float jitter_x = 0, float jitter_y = 0) {
	Mat4 r;
	r.At(0, 0) = p.x_scale;
	r.At(1, 1) = p.y_scale;
	r.At(2, 0) = jitter_x;
	r.At(2, 1) = jitter_y;
	r.At(2, 2) = p.z_scale;
	r.At(2, 3) = 1.0f;
	r.At(3, 2) = p.z_offset;
	return r;
}

// Clip space of this frame to clip space of the previous one: clip -> view -> world (this frame's
// camera) -> view -> clip (the previous camera). With identity world_to_view on both (no camera
// found) only a change of projection remains.
[[nodiscard]] inline std::optional<Mat4> ClipToPrevClip(const Mat4& view_to_clip, const Mat4& world_to_view,
                                                        const Mat4& prev_view_to_clip,
                                                        const Mat4& prev_world_to_view) {
	const auto clip_to_view  = Inverse(view_to_clip);
	const auto view_to_world = Inverse(world_to_view);
	if (!clip_to_view || !view_to_world) {
		return std::nullopt;
	}
	return Mul(Mul(Mul(*clip_to_view, *view_to_world), prev_world_to_view), prev_view_to_clip);
}

// A world-to-view matrix's camera: position and axes in world space (rows of its inverse).
struct CameraBasis {
	std::array<float, 3> position {}, right {}, up {}, forward {};
};
[[nodiscard]] inline std::optional<CameraBasis> CameraFromView(const Mat4& world_to_view) {
	const auto view_to_world = Inverse(world_to_view);
	if (!view_to_world) {
		return std::nullopt;
	}
	CameraBasis basis;
	for (int i = 0; i < 3; i++) {
		basis.right[static_cast<size_t>(i)]    = view_to_world->At(0, i);
		basis.up[static_cast<size_t>(i)]       = view_to_world->At(1, i);
		basis.forward[static_cast<size_t>(i)]  = view_to_world->At(2, i);
		basis.position[static_cast<size_t>(i)] = view_to_world->At(3, i);
	}
	return basis;
}

// A rigid world-to-view matrix (row vectors): orthonormal 3x3, last column (0, 0, 0, 1), a finite
// translation. The identity is refused (an unset matrix, not a camera).
[[nodiscard]] inline bool IsRigidView(const Mat4& a) {
	if (!a.Finite()) {
		return false;
	}
	constexpr float Tolerance = 2e-3f;
	if (std::abs(a.At(0, 3)) > 1e-5f || std::abs(a.At(1, 3)) > 1e-5f || std::abs(a.At(2, 3)) > 1e-5f ||
	    std::abs(a.At(3, 3) - 1.0f) > 1e-5f) {
		return false;
	}
	for (int i = 0; i < 3; i++) {
		for (int j = i; j < 3; j++) {
			float dot = 0;
			for (int k = 0; k < 3; k++) {
				dot += a.At(i, k) * a.At(j, k);
			}
			if (std::abs(dot - (i == j ? 1.0f : 0.0f)) > Tolerance) {
				return false;
			}
		}
	}
	for (int i = 0; i < 3; i++) {
		if (std::abs(a.At(3, i)) > 1e6f) {
			return false;
		}
	}
	bool identity = true;
	for (int i = 0; i < 16 && identity; i++) {
		identity = a.m[static_cast<size_t>(i)] == Mat4::Identity().m[static_cast<size_t>(i)];
	}
	return !identity;
}

// KYTY_DLSSG_CAMERA_SCAN: where a draw's constant buffer holds the game's projection, and a
// world-to-view matrix near it. Offsets are in bytes; matrices sit at 16-byte steps.
struct CameraMatch {
	size_t                projection_offset = 0;
	bool                  projection_transposed = false; // stored column by column
	bool                  y_flipped             = false; // the matrix negates y (Vulkan clip space)
	bool                  looks_down_minus_z    = false; // right-handed view space
	int                   score                 = 0;
	std::optional<size_t> view_offset;
	bool                  view_transposed = false;
};

inline constexpr int CameraScoreShape      = 10; // x/y scales, w = +-z, zeros, small jitter
inline constexpr int CameraScoreDepthTerm  = 2;  // each of z_scale and z_offset matching
inline constexpr int CameraScoreView       = 4;  // a rigid world-to-view matrix within the window
inline constexpr int CameraScoreUsable     = CameraScoreShape + 2 * CameraScoreDepthTerm;

namespace Detail {

[[nodiscard]] inline bool Near(float value, float expected, float relative) {
	return std::abs(value - expected) <= relative * std::abs(expected);
}

// Score of one candidate (0 when the shape does not match).
[[nodiscard]] inline int ProjectionScore(const Mat4& a, const Projection& expected, CameraMatch& match) {
	if (!a.Finite()) {
		return 0;
	}
	constexpr float Scale  = 0.01f;  // 1%: rounding of the shader's own constants
	constexpr float Zero   = 1e-4f;
	constexpr float Jitter = 0.01f;  // half a pixel is under 0.0032 at 320 pixels
	if (!Near(a.At(0, 0), expected.x_scale, Scale) || !Near(std::abs(a.At(1, 1)), expected.y_scale, Scale)) {
		return 0;
	}
	if (std::abs(std::abs(a.At(2, 3)) - 1.0f) > Zero) {
		return 0;
	}
	for (const auto [row, col] : {std::pair {0, 1}, std::pair {0, 2}, std::pair {0, 3}, std::pair {1, 0},
	                              std::pair {1, 2}, std::pair {1, 3}, std::pair {3, 0}, std::pair {3, 1},
	                              std::pair {3, 3}}) {
		if (std::abs(a.At(row, col)) > Zero) {
			return 0;
		}
	}
	if (std::abs(a.At(2, 0)) > Jitter || std::abs(a.At(2, 1)) > Jitter) {
		return 0;
	}
	const float hand          = a.At(2, 3) > 0 ? 1.0f : -1.0f;
	match.y_flipped           = a.At(1, 1) < 0;
	match.looks_down_minus_z  = hand < 0;
	int score                 = CameraScoreShape;
	score += Near(a.At(2, 2), hand * expected.z_scale, Scale) ? CameraScoreDepthTerm : 0;
	score += Near(a.At(3, 2), expected.z_offset, Scale) ? CameraScoreDepthTerm : 0;
	return score;
}

} // namespace Detail

// The best match in a buffer (16-byte aligned words), or nullopt. view_window: how far (bytes) on
// either side of the projection a world-to-view matrix is looked for.
[[nodiscard]] inline std::optional<CameraMatch> ScanForCamera(std::span<const uint32_t> words,
                                                              const Projection&         expected,
                                                              size_t                    view_window = 256) {
	std::optional<CameraMatch> best;
	if (!expected.Valid() || words.size() < 16) {
		return best;
	}
	for (size_t start = 0; start + 16 <= words.size(); start += 4) {
		for (const bool transposed : {false, true}) {
			CameraMatch match;
			const auto  matrix = Mat4::FromWords(words.subspan(start).first<16>(), transposed);
			match.score        = Detail::ProjectionScore(matrix, expected, match);
			if (match.score == 0) {
				continue;
			}
			match.projection_offset     = start * 4;
			match.projection_transposed = transposed;
			const size_t window         = view_window / 4;
			const size_t first          = start > window ? start - window : 0;
			for (size_t view = first; view + 16 <= words.size() && view <= start + window && !match.view_offset;
			     view += 4) {
				if (view + 16 > start && view < start + 16) {
					continue; // overlaps the projection
				}
				for (const bool view_transposed : {false, true}) {
					if (IsRigidView(Mat4::FromWords(words.subspan(view).first<16>(), view_transposed))) {
						match.view_offset     = view * 4;
						match.view_transposed = view_transposed;
						match.score += CameraScoreView;
						break;
					}
				}
			}
			if (!best || match.score > best->score) {
				best = match;
			}
		}
	}
	return best;
}

// The game's world-to-view matrix in this file's convention (+z forward, the y of ViewToClip): the
// game's projection is ViewToClip's with rows 1 / 2 negated where it flips y / looks down -z, so
// view * game_projection == (view * diag(1, sy, sz, 1)) * ViewToClip(projection).
[[nodiscard]] inline Mat4 ViewFromGame(const Mat4& game_view, bool y_flipped, bool looks_down_minus_z) {
	Mat4 r = game_view;
	for (int row = 0; row < 4; row++) {
		if (y_flipped) {
			r.At(row, 1) = -r.At(row, 1);
		}
		if (looks_down_minus_z) {
			r.At(row, 2) = -r.At(row, 2);
		}
	}
	return r;
}

// KYTY_DLSSG_CAMERA_SCAN: finds the constant buffer slot holding the camera, then reads the view from
// it once per frame. GPU thread only; `frame` counts guest flips. Searching: every scan_every-th
// frame, at most max_buffers buffers; a (shader, slot, layout) locks once its view was seen in
// confirm_frames scanned frames and never differed between draws of one frame (per-object or
// second-camera buffers do). After the lock the last good view stays when the shader is not drawn.
class CameraTracker {
public:
	static constexpr uint32_t ScanEvery     = 15;
	static constexpr uint32_t MaxBuffers    = 256;
	static constexpr uint32_t ConfirmFrames = 3;
	static constexpr size_t   MaxBytes      = 4096;
	static constexpr size_t   MaxCandidates = 32;

	struct Candidate {
		uint64_t    shader = 0;
		uint32_t    slot   = 0;
		uint64_t    size   = 0;
		CameraMatch match;
		uint32_t    frames     = 0;
		uint64_t    last_frame = 0;
		uint64_t    view_hash  = 0;
		bool        varies     = false;
	};
	enum class Event : uint8_t { None, Candidate, Varies, Locked };

	void SetExpected(const Projection& expected) { m_expected = expected; }

	// Whether to read this buffer (at most MaxBytes of it) and Feed it.
	[[nodiscard]] bool Wants(uint64_t frame, uint64_t shader, uint32_t slot, uint64_t size) {
		if (m_lock) {
			return shader == m_lock->shader && slot == m_lock->slot && frame != m_read_frame &&
			       size >= *m_lock->match.view_offset + 64;
		}
		if (!m_expected.Valid() || frame % ScanEvery != 0 || size < 64 || size > MaxBytes) {
			return false;
		}
		if (frame != m_budget_frame) {
			m_budget_frame = frame;
			m_budget_used  = 0;
		}
		return m_budget_used++ < MaxBuffers;
	}

	// The buffer's words (16-byte aligned length). `event` tells what changed; `candidate` is the
	// candidate it concerns (for the log).
	Event Feed(uint64_t frame, uint64_t shader, uint32_t slot, uint64_t size, std::span<const uint32_t> words,
	           const Candidate** candidate = nullptr) {
		if (m_lock) {
			const size_t first = *m_lock->match.view_offset / 4;
			if (first + 16 > words.size()) {
				return Event::None;
			}
			// Checked as the game stores it (as the scanner did): an unset identity stays refused.
			const auto game_view = Mat4::FromWords(words.subspan(first).first<16>(), m_lock->match.view_transposed);
			if (IsRigidView(game_view)) {
				m_view       = ViewFromGame(game_view, m_lock->match.y_flipped, m_lock->match.looks_down_minus_z);
				m_read_frame = frame;
				m_reads++;
			} else {
				m_rejects++;
			}
			return Event::None;
		}
		const auto match = ScanForCamera(words, m_expected);
		if (!match || match->score < CameraScoreUsable || !match->view_offset) {
			return Event::None;
		}
		const uint64_t hash = HashWords(words.subspan(*match->view_offset / 4).first<16>());
		auto           it   = std::find_if(m_candidates.begin(), m_candidates.end(), [&](const Candidate& c) {
            return c.shader == shader && c.slot == slot && SameLayout(c.match, *match);
        });
		Event event = Event::None;
		if (it == m_candidates.end()) {
			if (m_candidates.size() >= MaxCandidates) {
				return Event::None;
			}
			m_candidates.push_back({shader, slot, size, *match, 1, frame, hash, false});
			it    = m_candidates.end() - 1;
			event = Event::Candidate;
		} else if (it->last_frame == frame) {
			if (it->view_hash != hash && !it->varies) {
				it->varies = true;
				event      = Event::Varies;
			}
		} else {
			it->frames++;
			it->last_frame = frame;
			it->view_hash  = hash;
		}
		if (candidate != nullptr) {
			*candidate = &*it;
		}
		if (!it->varies && it->frames >= ConfirmFrames && event == Event::None) {
			m_lock = *it;
			m_view = ViewFromGame(Mat4::FromWords(words.subspan(*match->view_offset / 4).first<16>(),
			                                      match->view_transposed),
			                      match->y_flipped, match->looks_down_minus_z);
			m_read_frame = frame;
			return Event::Locked;
		}
		return event;
	}

	[[nodiscard]] const std::optional<Candidate>& Lock() const { return m_lock; }
	// This file's convention; identity until the lock.
	[[nodiscard]] const Mat4& View() const { return m_view; }
	[[nodiscard]] uint64_t    Reads() const { return m_reads; }
	[[nodiscard]] uint64_t    Rejects() const { return m_rejects; }

private:
	static bool SameLayout(const CameraMatch& a, const CameraMatch& b) {
		return a.projection_offset == b.projection_offset && a.projection_transposed == b.projection_transposed &&
		       a.view_offset == b.view_offset && a.view_transposed == b.view_transposed &&
		       a.y_flipped == b.y_flipped && a.looks_down_minus_z == b.looks_down_minus_z;
	}
	static uint64_t HashWords(std::span<const uint32_t, 16> words) {
		uint64_t hash = 1469598103934665603ull; // FNV-1a
		for (const uint32_t word : words) {
			hash = (hash ^ word) * 1099511628211ull;
		}
		return hash;
	}

	Projection               m_expected {};
	std::vector<Candidate>   m_candidates;
	std::optional<Candidate> m_lock;
	Mat4                     m_view         = Mat4::Identity();
	uint64_t                 m_read_frame   = ~0ull;
	uint64_t                 m_budget_frame = ~0ull;
	uint32_t                 m_budget_used  = 0;
	uint64_t                 m_reads = 0, m_rejects = 0;
};

} // namespace Libs::Graphics::FrameGen
