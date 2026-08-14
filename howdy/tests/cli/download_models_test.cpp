#include "cli/download_models_test_support.hpp"

#include <algorithm>
#include <array>

#include <sys/stat.h>

namespace {

	struct UmaskGuard {
		mode_t previous;

		explicit UmaskGuard(mode_t value)
		    : previous(umask(value)) {}

		UmaskGuard(const UmaskGuard &)                     = delete;
		auto operator=(const UmaskGuard &) -> UmaskGuard & = delete;

		~UmaskGuard() {
			umask(previous);
		}
	};

}  // namespace

auto main() -> int {
	using namespace howdy::test::download_models;
	const UmaskGuard umask_guard(0022);
	const std::array results = {run_download_models_entrypoint_tests(), run_atomic_files_tests(),
	                            run_download_models_integrity_tests(),
	                            run_download_models_manifest_tests()};
	const bool       ok      = std::ranges::all_of(results, [](bool value) -> bool {
		return value;
	});
	return ok ? 0 : 1;
}
