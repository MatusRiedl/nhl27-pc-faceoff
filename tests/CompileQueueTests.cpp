// Tests of the shader compile workers' queue (src/graphics/host_gpu/renderer/pipeline/compileQueue.h,
// v0.30.0 KYTY_COMPILE_URGENT_FIRST). No GPU and no emulator: the queue is header-only.
#include "graphics/host_gpu/renderer/pipeline/compileQueue.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <random>
#include <thread>
#include <vector>

namespace {

using Libs::Graphics::CompileClass;
using Libs::Graphics::CompileLevel;

struct Job {
	int id  = 0;
	int key = 0;
};
using Queue = Libs::Graphics::CompileQueue<int, Job>;

std::atomic<int> g_failures {0};

#define CHECK(condition)                                                                   \
	do {                                                                                   \
		if (!(condition)) {                                                                \
			std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
			g_failures++;                                                                  \
		}                                                                                  \
	} while (false)

void Push(Queue& queue, int id, int key, CompileClass job_class) {
	queue.Push(key, std::make_unique<Job>(Job {.id = id, .key = key}), job_class);
}

// Pops every queued job (the queue must not be empty, or Pop blocks).
std::vector<int> Drain(Queue& queue) {
	std::vector<int> ids;
	while (!queue.Empty()) {
		ids.push_back(queue.Pop()->id);
	}
	return ids;
}

void OneClassIsFifo() {
	Queue queue;
	CHECK(!queue.TwoClasses());
	Push(queue, 1, 10, CompileClass::Stored);
	Push(queue, 2, 20, CompileClass::Live);
	Push(queue, 3, 10, CompileClass::Stored);
	Push(queue, 4, 30, CompileClass::Live);
	// The queue as it was: no take-back, no promotion.
	CHECK(queue.Take(20) == nullptr);
	CHECK(queue.Promote(10, CompileLevel::Urgent) == 0);
	CHECK(queue.Promote(30, CompileLevel::Live) == 0);
	CHECK((Drain(queue) == std::vector<int> {1, 2, 3, 4}));
	const auto stats = queue.Stats();
	CHECK(stats.taken == 0 && stats.promoted == 0);
}

void LiveBeforeStored() {
	Queue queue(true);
	CHECK(queue.TwoClasses());
	Push(queue, 1, 10, CompileClass::Stored);
	Push(queue, 2, 11, CompileClass::Stored);
	Push(queue, 3, 20, CompileClass::Live);
	Push(queue, 4, 12, CompileClass::Stored);
	Push(queue, 5, 21, CompileClass::Live);
	CHECK((Drain(queue) == std::vector<int> {3, 5, 1, 2, 4}));
}

void TakeOnlyQueuedLiveJobs() {
	Queue queue(true);
	Push(queue, 1, 10, CompileClass::Stored);
	Push(queue, 2, 20, CompileClass::Live);
	Push(queue, 3, 30, CompileClass::Live);
	// Stored jobs are never taken back.
	CHECK(queue.Take(10) == nullptr);
	// A key with no job.
	CHECK(queue.Take(99) == nullptr);
	const auto taken = queue.Take(30);
	CHECK(taken != nullptr && taken->id == 3);
	// Taken once only.
	CHECK(queue.Take(30) == nullptr);
	// A job a worker has popped (runs) cannot be taken.
	const auto running = queue.Pop();
	CHECK(running != nullptr && running->id == 2);
	CHECK(queue.Take(20) == nullptr);
	CHECK((Drain(queue) == std::vector<int> {1}));
	CHECK(queue.Stats().taken == 1);
}

void PromoteLiveAndUrgent() {
	Queue queue(true);
	Push(queue, 1, 10, CompileClass::Stored);
	Push(queue, 2, 11, CompileClass::Stored);
	Push(queue, 3, 10, CompileClass::Stored);
	Push(queue, 4, 12, CompileClass::Stored);
	Push(queue, 5, 20, CompileClass::Live);
	// Key 10's stored jobs leave the backlog to the end of the live jobs, in their order.
	CHECK(queue.Promote(10, CompileLevel::Live) == 2);
	// Already live: nothing to move.
	CHECK(queue.Promote(10, CompileLevel::Live) == 0);
	CHECK(queue.Promote(20, CompileLevel::Live) == 0);
	CHECK(queue.Promote(99, CompileLevel::Urgent) == 0);
	// Key 12 to the front of everything.
	CHECK(queue.Promote(12, CompileLevel::Urgent) == 1);
	// A job is never moved back.
	CHECK(queue.Promote(12, CompileLevel::Live) == 0);
	// The key asked for last goes in front of urgent jobs promoted earlier.
	CHECK(queue.Promote(11, CompileLevel::Urgent) == 1);
	// A stored job, even urgent, is still not taken back.
	CHECK(queue.Take(11) == nullptr);
	CHECK((Drain(queue) == std::vector<int> {2, 4, 5, 1, 3}));
	CHECK(queue.Stats().promoted == 4);
}

void UrgentKeepsOrderOfOneKey() {
	Queue queue(true);
	Push(queue, 1, 10, CompileClass::Stored);
	Push(queue, 2, 11, CompileClass::Stored);
	Push(queue, 3, 10, CompileClass::Stored);
	Push(queue, 4, 10, CompileClass::Stored);
	Push(queue, 5, 20, CompileClass::Live);
	CHECK(queue.Promote(10, CompileLevel::Urgent) == 3);
	CHECK((Drain(queue) == std::vector<int> {1, 3, 4, 5, 2}));
}

void ManyJobsOfOneKey() {
	// More jobs of one key than one Promote call moves: the rest move on the next call.
	Queue queue(true);
	for (int id = 0; id < 100; id++) {
		Push(queue, id, 10, CompileClass::Stored);
	}
	Push(queue, 1000, 20, CompileClass::Live);
	size_t moved = 0;
	for (size_t step; (step = queue.Promote(10, CompileLevel::Urgent)) != 0;) {
		moved += step;
	}
	CHECK(moved == 100);
	const auto ids = Drain(queue);
	CHECK(ids.size() == 101);
	CHECK(ids.back() == 1000);
}

void StatsCountWaitingJobs() {
	Queue queue(true);
	Push(queue, 1, 10, CompileClass::Stored);
	Push(queue, 2, 20, CompileClass::Live);
	Push(queue, 3, 30, CompileClass::Live);
	auto stats = queue.Stats();
	CHECK(stats.live_left == 2 && stats.stored_left == 1 && stats.live_max == 2);
	CHECK(queue.Promote(10, CompileLevel::Urgent) == 1);
	stats = queue.Stats();
	CHECK(stats.live_left == 3 && stats.stored_left == 0 && stats.live_max == 3);
	(void)Drain(queue);
	stats = queue.Stats();
	CHECK(stats.live_left == 0 && stats.stored_left == 0 && stats.live_max == 3);
}

void StopWakesWorkers() {
	Queue             queue(true);
	std::atomic<int>  returned {0};
	std::vector<std::thread> workers;
	for (int index = 0; index < 4; index++) {
		workers.emplace_back([&] {
			if (queue.Pop() == nullptr) {
				returned++;
			}
		});
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	queue.Stop();
	for (auto& worker: workers) {
		worker.join();
	}
	CHECK(returned == 4);
	// Jobs queued after Stop are not handed out (the destructor frees them).
	Push(queue, 1, 10, CompileClass::Live);
	CHECK(queue.Pop() == nullptr);
}

// Workers pop, a resolve thread pushes, an execution thread takes back and promotes at random:
// every job is run by a worker or taken back, exactly once.
void Stress(bool two_classes) {
	constexpr int Jobs    = 20000;
	constexpr int Workers = 6;
	Queue         queue(two_classes);
	std::vector<std::atomic<int>> seen(Jobs);
	std::atomic<int>              pushed {0};
	std::atomic<bool>             done {false};

	std::vector<std::thread> workers;
	for (int index = 0; index < Workers; index++) {
		workers.emplace_back([&] {
			while (auto job = queue.Pop()) {
				seen[job->id]++;
			}
		});
	}
	std::thread resolve([&] {
		for (int id = 0; id < Jobs; id++) {
			// A key has either one live job or several stored jobs (as in pipelineCache.cpp).
			const bool stored = id < Jobs / 2;
			const int  key    = stored ? id % 997 : 100000 + id;
			Push(queue, id, key, stored ? CompileClass::Stored : CompileClass::Live);
			pushed = id + 1;
		}
	});
	std::thread execution([&] {
		std::mt19937 random(1234);
		while (!done) {
			const int limit = pushed.load();
			if (limit == 0) {
				continue;
			}
			const int id     = static_cast<int>(random() % static_cast<uint32_t>(limit));
			const bool stored = id < Jobs / 2;
			const int key     = stored ? id % 997 : 100000 + id;
			if (auto job = queue.Take(key)) {
				CHECK(!stored);
				seen[job->id]++;
			} else {
				(void)queue.Promote(key, (random() & 1) != 0 ? CompileLevel::Urgent : CompileLevel::Live);
			}
		}
	});
	resolve.join();
	const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	while (!queue.Empty() && std::chrono::steady_clock::now() < until) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	done = true;
	execution.join();
	// Workers finish the job in hand before Pop returns null, and join orders their counts.
	queue.Stop();
	for (auto& worker: workers) {
		worker.join();
	}
	int missing = 0;
	int twice   = 0;
	for (const auto& count: seen) {
		missing += count == 0;
		twice += count > 1;
	}
	if (missing != 0 || twice != 0) {
		std::fprintf(stderr, "stress (two_classes=%d): %d jobs never ran, %d ran more than once\n", two_classes ? 1 : 0,
		             missing, twice);
		g_failures++;
	}
	if (!two_classes) {
		CHECK(queue.Stats().taken == 0);
	}
}

} // namespace

int main() {
	OneClassIsFifo();
	LiveBeforeStored();
	TakeOnlyQueuedLiveJobs();
	PromoteLiveAndUrgent();
	UrgentKeepsOrderOfOneKey();
	ManyJobsOfOneKey();
	StatsCountWaitingJobs();
	StopWakesWorkers();
	Stress(false);
	Stress(true);
	if (g_failures != 0) {
		std::fprintf(stderr, "%d compile queue checks failed\n", g_failures.load());
		return 1;
	}
	std::puts("Compile queue tests passed");
	return 0;
}
