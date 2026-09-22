#include "support/cpu_topology.hpp"
#include "test_support.hpp"

#include <cstddef>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <system_error>

#include <sched.h>

namespace {

	using howdy::test::Expect;
	using howdy::test::WriteFile;

	auto MakeAffinity(std::initializer_list<std::size_t> cpus) -> cpu_set_t {
		cpu_set_t affinity{};
		CPU_ZERO(&affinity);
		for (const std::size_t cpu : cpus) {
			CPU_SET(cpu, &affinity);
		}
		return affinity;
	}

	auto WriteSiblingList(const std::filesystem::path &root, int cpu, const std::string &siblings)
	    -> bool {
		std::error_code ec;
		const auto      topology = root / ("cpu" + std::to_string(cpu)) / "topology";
		std::filesystem::create_directories(topology, ec);
		return !ec && WriteFile(topology / "thread_siblings_list", siblings + "\n");
	}

}  // namespace

auto main() -> int {
	namespace fs = std::filesystem;

	bool            ok = true;
	std::error_code ec;
	const auto      temp_root = fs::current_path() / "howdy-cpu-topology-test";
	fs::remove_all(temp_root, ec);
	fs::create_directories(temp_root, ec);
	ok &= Expect(!ec, "create temporary CPU topology tree");

	for (int cpu = 0; cpu < 8; cpu += 2) {
		const auto siblings = std::to_string(cpu) + "-" + std::to_string(cpu + 1);
		ok &= Expect(WriteSiblingList(temp_root, cpu, siblings), "write first SMT sibling list");
		ok &=
		    Expect(WriteSiblingList(temp_root, cpu + 1, siblings), "write second SMT sibling list");
	}

	ok &= Expect(howdy::native::cpu_topology_detail::CountPhysicalCores(
	                 MakeAffinity({0, 1, 2, 3, 4, 5, 6, 7}), temp_root) == 4,
	             "eight logical CPUs across four SMT cores count as four physical cores");
	ok &= Expect(howdy::native::cpu_topology_detail::CountPhysicalCores(MakeAffinity({0, 2, 4, 6}),
	                                                                    temp_root) == 4,
	             "one allowed thread from each physical core still counts four cores");
	ok &= Expect(howdy::native::cpu_topology_detail::CountPhysicalCores(MakeAffinity({0, 1}),
	                                                                    temp_root) == 1,
	             "two SMT siblings count as one physical core");

	ok &= Expect(howdy::native::cpu_topology_detail::CountPhysicalCores(MakeAffinity({0, 1, 4, 5}),
	                                                                    temp_root) == 2,
	             "sparse affinity counts only physical cores containing allowed CPUs");
	ok &= Expect(
	    !howdy::native::cpu_topology_detail::CountPhysicalCores(MakeAffinity({0, 1, 8}), temp_root)
	         .has_value(),
	    "missing topology leaves the OpenCV thread default unchanged");
	ok &=
	    Expect(!howdy::native::cpu_topology_detail::CountPhysicalCores(MakeAffinity({}), temp_root)
	                .has_value(),
	           "empty affinity leaves the OpenCV thread default unchanged");

	fs::remove_all(temp_root, ec);
	return ok ? 0 : 1;
}
