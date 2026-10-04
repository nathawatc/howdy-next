#include "cli/download_models_test_support.hpp"

#include <algorithm>
#include <array>

#include <sys/stat.h>
#include <sys/types.h>

namespace {

	class UmaskGuard {
	public:
		UmaskGuard(UmaskGuard &&)                     = delete;
		auto operator=(UmaskGuard &&) -> UmaskGuard & = delete;

		explicit UmaskGuard(mode_t value)
		    : previous_(umask(value)) {}

		UmaskGuard(const UmaskGuard &)                     = delete;
		auto operator=(const UmaskGuard &) -> UmaskGuard & = delete;

		~UmaskGuard() {
			umask(previous_);
		}

	private:
		mode_t previous_;
	};

}  // namespace

auto main() -> int {
	using howdy::test::download_models::RunAtomicFilesTests;
	using howdy::test::download_models::RunDownloadModelsEntrypointTests;
	using howdy::test::download_models::RunDownloadModelsIntegrityTests;
	using howdy::test::download_models::RunDownloadModelsManifestTests;
	using howdy::test::download_models::RunDownloadModelsProxyTests;
	const UmaskGuard umask_guard(0022);
	const std::array results = {
	    RunDownloadModelsEntrypointTests(), RunAtomicFilesTests(),
	    RunDownloadModelsIntegrityTests(),  RunDownloadModelsManifestTests(),
	    RunDownloadModelsProxyTests(),
	};
	const bool ok = std::ranges::all_of(results, [](bool value) -> bool {
		return value;
	});
	return ok ? 0 : 1;
}
