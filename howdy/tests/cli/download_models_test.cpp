#include "cli/download_models_test_support.hpp"

#include <algorithm>
#include <array>

auto main() -> int {
	using howdy::test::download_models::RunAtomicFilesTests;
	using howdy::test::download_models::RunDownloadModelsEntrypointTests;
	using howdy::test::download_models::RunDownloadModelsIntegrityTests;
	using howdy::test::download_models::RunDownloadModelsManifestTests;
	using howdy::test::download_models::RunDownloadModelsProxyTests;
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
