#ifndef KYTY_COMMON_HOST_STATS_H_
#define KYTY_COMMON_HOST_STATS_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Common {

enum class MemoryPressure { Unknown, Normal, Warning, Critical };
enum class ThermalPressure { Unknown, Nominal, Moderate, Heavy, Critical };

struct ThreadUsage {
	std::string name;
	double      cpu_percent = 0.0; // Of one core.
};

// Host-wide and process-wide resource usage. Missing values are std::nullopt; rates are per
// second since the previous sample.
struct HostStats {
	uint32_t logical_cores = 0;

	// Processing.
	std::optional<double>    process_cpu_percent; // Share of all logical cores, 0-100.
	std::optional<double>    system_cpu_percent;  // Whole machine, 0-100.
	std::optional<uint32_t>  thread_count;
	std::vector<ThreadUsage> busiest_threads; // Busiest first.
	std::optional<double>    context_switches_per_s;
	std::optional<double>    page_faults_per_s;
	std::optional<double>    page_ins_per_s; // Faults that read from disk.
	ThermalPressure          thermal_pressure = ThermalPressure::Unknown;

	// Emulator process memory.
	std::optional<uint64_t> process_memory; // Physical footprint / resident set.
	std::optional<uint64_t> process_resident;
	std::optional<uint64_t> process_compressed; // Compressed or swapped out.
	std::optional<uint64_t> process_file_backed;
	std::optional<uint64_t> process_virtual; // Reserved address space.

	// System memory.
	std::optional<uint64_t> system_memory_used;
	std::optional<uint64_t> system_memory_total;
	std::optional<uint64_t> system_wired;
	std::optional<uint64_t> system_compressed;
	std::optional<uint64_t> system_cached; // File cache, reclaimable.
	std::optional<uint64_t> system_free;
	std::optional<uint64_t> swap_used;
	std::optional<uint64_t> swap_total;
	MemoryPressure          memory_pressure = MemoryPressure::Unknown;

	// Disk I/O of the emulator process.
	std::optional<double> disk_read_per_s;
	std::optional<double> disk_write_per_s;

	// GPU, whole device (macOS IOAccelerator).
	std::string             gpu_model;
	std::optional<uint32_t> gpu_cores;
	std::optional<double>   gpu_percent;
	std::optional<double>   gpu_renderer_percent;
	std::optional<double>   gpu_tiler_percent;
	std::optional<uint64_t> gpu_memory_in_use; // System memory the GPU driver has in use.
	std::optional<uint64_t> gpu_memory_allocated;
	std::optional<uint64_t> gpu_recoveries; // GPU resets since boot.
};

// May block for a few milliseconds (thread enumeration, OS registry reads), so call it off the
// render thread. The first sample has no rates.
class HostStatsSampler final {
public:
	HostStatsSampler();
	~HostStatsSampler();
	HostStatsSampler(const HostStatsSampler&)            = delete;
	HostStatsSampler& operator=(const HostStatsSampler&) = delete;

	[[nodiscard]] HostStats Sample();

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

// Windows commit charge (zeros elsewhere or when the query fails): the system's commit in use and
// limit (RAM plus paging files), and this process's private commit, in bytes.
struct CommitCharge {
	uint64_t system_used  = 0;
	uint64_t system_limit = 0;
	uint64_t process      = 0;
};
[[nodiscard]] CommitCharge GetCommitCharge() noexcept;

// NHL27OOM: an allocation of requested_bytes failed; prints the commit charge and what to do.
void PrintOutOfCommit(const char* where, uint64_t requested_bytes) noexcept;

// NHL27POWER (KYTY_POWER_THROTTLING_OPTOUT, default 1): asks Windows never to power-throttle this
// process (EcoQoS while the window is unfocused or covered, ignored timer resolution).
void OptOutOfPowerThrottling() noexcept;

} // namespace Common

#endif /* KYTY_COMMON_HOST_STATS_H_ */
