// NHL27HITCHWHY d_cpu / c_cpu (KYTY_HITCH_CPU=1): CpuRegisterCurrent and CpuTake against real threads.
// A "draw" thread spins (on the CPU all the time), a "command" thread sleeps (off the CPU almost all the
// time); the reader takes samples every 100 ms, as the presenting thread does every frame.

#include "common/hitchWhy.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

#if defined(_WIN32)
#include <stdlib.h>
#endif

namespace {

using Common::HitchWhy::CpuRegisterCurrent;
using Common::HitchWhy::CpuTake;
using Common::HitchWhy::Role;

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "HitchCpuTests: failed: %s\n", text);
		std::abort();
	}
}

std::atomic<bool> g_stop {false};
std::atomic<int>  g_registered {0};

void Spin() {
	CpuRegisterCurrent(Role::Draw);
	g_registered.fetch_add(1);
	volatile uint64_t sink = 0;
	while (!g_stop.load(std::memory_order_relaxed)) {
		sink = sink + 1;
	}
}

void Sleeper() {
	CpuRegisterCurrent(Role::Cp);
	g_registered.fetch_add(1);
	while (!g_stop.load(std::memory_order_relaxed)) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
}

} // namespace

int main() {
#if defined(_WIN32)
	_putenv_s("KYTY_HITCH_CPU", "1");
	Check(Common::HitchWhy::CpuEnabled(), "switch read");
	// Roles other than the draw and command thread are ignored.
	CpuRegisterCurrent(Role::Present);
	double draw = 0.0, cp = 0.0, cyc = 0.0;
	Check(!CpuTake(&draw, &cp, &cyc), "nothing registered");

	std::thread spin(Spin);
	std::thread sleeper(Sleeper);
	while (g_registered.load() < 2) {
		std::this_thread::yield();
	}
	int    results     = 0;
	double draw_sum    = 0.0;
	double cp_sum      = 0.0;
	double wall_ms_sum = 0.0;
	auto   last        = std::chrono::steady_clock::now();
	for (int i = 0; i < 40; i++) { // 4 s: about 1 s of CPU passes before the first result
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		const bool ok  = CpuTake(&draw, &cp, &cyc);
		const auto now = std::chrono::steady_clock::now();
		const double wall_ms = std::chrono::duration<double, std::milli>(now - last).count();
		last                 = now;
		if (!ok) {
			Check(results == 0, "a result, then none");
			continue;
		}
		results++;
		draw_sum += draw;
		cp_sum += cp;
		wall_ms_sum += wall_ms;
		Check(cp >= 0.0, "command thread seen");
	}
	g_stop.store(true);
	spin.join();
	sleeper.join();
	std::printf("HitchCpuTests: results=%d draw=%.1f ms cp=%.1f ms wall=%.1f ms cyc_per_us=%.0f\n", results, draw_sum,
	            cp_sum, wall_ms_sum, cyc);
	Check(results >= 20, "calibrated within about 1.5 s");
	// The spinning thread is on the CPU nearly the whole wall time (a busy PC may take some of it).
	Check(draw_sum > 0.6 * wall_ms_sum && draw_sum < 1.15 * wall_ms_sum, "draw on-CPU close to wall time");
	// The sleeping thread is off the CPU nearly all the time.
	Check(cp_sum < 0.1 * wall_ms_sum, "command on-CPU small");
	// A cycle counter in a plausible range for a desktop CPU (0.5-8 GHz).
	Check(cyc > 500.0 && cyc < 8000.0, "cycles per microsecond");
#else
	double draw = 0.0, cp = 0.0, cyc = 0.0;
	Check(!CpuTake(&draw, &cp, &cyc), "off outside Windows");
#endif
	std::printf("HitchCpuTests: all passed\n");
	return 0;
}
