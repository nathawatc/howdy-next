#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>

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

	inline auto AvailablePhysicalCoreCount() -> std::optional<int> {
		cpu_set_t affinity{};
		CPU_ZERO(&affinity);
		if (sched_getaffinity(0, sizeof(affinity), &affinity) != 0) {
			return std::nullopt;
		}
		return cpu_topology_detail::CountPhysicalCores(affinity, "/sys/devices/system/cpu");
	}

}  // namespace howdy::native
