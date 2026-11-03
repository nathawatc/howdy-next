#pragma once

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <thread>

#include <sched.h>

namespace howdy::native {

	namespace cpu_topology_detail {

		inline auto CountPhysicalCores(const cpu_set_t             &affinity,
		                               const std::filesystem::path &sys_cpu_root)
		    -> std::optional<int> {
			std::set<std::string> physical_cores;
			for (std::size_t cpu = 0; cpu < static_cast<std::size_t>(CPU_SETSIZE); ++cpu) {
				if (!CPU_ISSET(cpu, &affinity)) {
					continue;
				}

				std::ifstream siblings_file(sys_cpu_root / ("cpu" + std::to_string(cpu)) /
				                            "topology/thread_siblings_list");
				std::string   siblings;
				if (!(siblings_file >> siblings) || siblings.empty()) {
					return std::nullopt;
				}
				physical_cores.insert(siblings);
			}

			if (physical_cores.empty()) {
				return std::nullopt;
			}
			return static_cast<int>(physical_cores.size());
		}

	}  // namespace cpu_topology_detail

	// Threads OpenCV should use: the physical core count, or every allowed CPU when the topology
	// cannot be read (the same count OpenCV picks by default, so that case is unchanged). Never 0,
	// so compare always sets an explicit count; an OpenMP-backend OpenCV otherwise falls back to
	// OMP_NUM_THREADS, which compare pins to 1.
	inline auto ThreadCountFromAffinity(const cpu_set_t             &affinity,
	                                    const std::filesystem::path &sys_cpu_root) -> int {
		if (const auto physical_cores =
		        cpu_topology_detail::CountPhysicalCores(affinity, sys_cpu_root);
		    physical_cores.has_value()) {
			return *physical_cores;
		}
		return std::max(1, CPU_COUNT(&affinity));
	}

	inline auto AvailableThreadCount() -> int {
		cpu_set_t affinity{};
		CPU_ZERO(&affinity);
		if (sched_getaffinity(0, sizeof(affinity), &affinity) != 0) {
			return std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
		}
		return ThreadCountFromAffinity(affinity, "/sys/devices/system/cpu");
	}

}  // namespace howdy::native
