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
	const std::array results = {RunDownloadModelsEntrypointTests(), RunAtomicFilesTests(),
	                            RunDownloadModelsIntegrityTests(), RunDownloadModelsManifestTests(),
	                            RunDownloadModelsProxyTests()};
	const bool       ok      = std::ranges::all_of(results, [](bool value) -> bool {
		return value;
	});
	return ok ? 0 : 1;
}
