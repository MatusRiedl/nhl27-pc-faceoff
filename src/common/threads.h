#ifndef KYTY_COMMON_THREADS_H_
#define KYTY_COMMON_THREADS_H_

#include "common/common.h"

#include <memory>
#include <string>

namespace Common {

void InitializeThreads();

using thread_func_t    = void (*)(void*);
using wait_poll_func_t = void (*)();

struct ThreadPrivate;
struct MutexPrivate;
struct CondVarPrivate;

class Thread {
public:
	Thread(thread_func_t func, void* arg);
	~Thread();

	void Join();
	void Detach();

	// Once a thread has finished, the id may be reused by another thread.
	[[nodiscard]] std::string GetId() const;

	// The id is unique and can't be reused by another thread.
	[[nodiscard]] int GetUniqueId() const;

	static void SleepMicro(uint32_t micros);
	static void SleepNano(uint64_t nanos);
	static bool IsMainThread();
	// Current thread: raises its priority to "highest", and restricts it to one logical
	// processor (when `logical_cpu` is not negative). No-ops where unsupported.
	static void RaiseCurrentPriority();
	static void PinCurrent(int logical_cpu);

	// Processor group 0 as Windows reports it (GetLogicalProcessorInformationEx), read once.
	// Fast cores are the physical cores of the highest efficiency class: the P-cores of an
	// Intel hybrid CPU, every core elsewhere.
	struct CpuTopology {
		bool     valid      = false;
		bool     hybrid     = false;
		uint32_t logical    = 0;
		uint32_t cores      = 0;
		uint32_t fast_cores = 0;
		// Lowest logical CPU of each fast core, ascending.
		int      fast_core_first[64] {};
		// Per logical CPU: every logical CPU of its physical core (its SMT siblings and itself).
		uint64_t core_mask[64] {};
	};
	static const CpuTopology& GetCpuTopology();

	// Shader compile workers (pipelineCache.cpp): cores - 2, from 6 to 10, so the execution, resolve
	// and present threads keep cores of their own; 6 when the topology is unknown (the number
	// before 2026-11-19). env is KYTY_COMPILE_WORKERS: 1..16 sets the number, "0" gives the old fixed
	// 6, null or anything else keeps the rule. rule (optional) names where the number came from:
	// "env", "fixed", "cores" or "unknown".
	static uint32_t CompileWorkerCount(const CpuTopology& topology, const char* env, const char** rule = nullptr);

	// Get current thread id
	// Once a thread has finished, the id may be reused by another thread.
	static std::string GetThreadId();

	// Get current thread id
	// The id is unique and can't be reused by another thread.
	static int GetThreadIdUnique();

	KYTY_CLASS_NO_COPY(Thread);

private:
	std::unique_ptr<ThreadPrivate> m_thread;
};

class Mutex {
public:
	Mutex();
	~Mutex();

	void Lock();
	void Unlock();
	bool TryLock();

	friend class CondVar;

	KYTY_CLASS_NO_COPY(Mutex);

private:
	std::unique_ptr<MutexPrivate> m_mutex;
};

class CondVar {
public:
	CondVar();
	~CondVar();

	void Wait(Mutex* mutex);
	bool WaitFor(Mutex* mutex, uint32_t micros);
	void Signal();
	void SignalAll();

	static void SetWaitPollCallback(wait_poll_func_t callback);

	KYTY_CLASS_NO_COPY(CondVar);

private:
	std::unique_ptr<CondVarPrivate> m_cond_var;
};

class LockGuard {
public:
	using mutex_type = Mutex;

	// NOLINTNEXTLINE(google-runtime-references)
	explicit LockGuard(mutex_type& m): m_mutex(m) { m_mutex.Lock(); }

	~LockGuard() { m_mutex.Unlock(); }

	KYTY_CLASS_NO_COPY(LockGuard);

private:
	mutex_type& m_mutex;
};

} // namespace Common

#endif /* KYTY_COMMON_THREADS_H_ */
