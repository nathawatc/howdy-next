#pragma once

#include "cli/download_models/internal.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace howdy::test::download_models {

	using howdy::test::CountFilesWithPrefix;
	using howdy::test::ReadFile;
	using howdy::test::WriteFile;

	inline constexpr auto kTestModelContent = "small test model";
	inline constexpr auto kTestModelSha256 =
	    "eceb1d87ddd7b5c0e1b63bdac2d086824ffd12eaa157b741835014618a1e6c24";
	inline constexpr howdy::native::OpenCvModelDescriptor kTestModel{
	    .type     = howdy::native::OpenCvModelType::kYunet,
	    .filename = "test-model.onnx",
	    .url      = "https://example.invalid/test-model.onnx",
	    .sha256   = kTestModelSha256,
	    .size     = std::string_view(kTestModelContent).size(),
	};

	extern int                      download_attempts;
	extern std::string              downloaded_content;
	extern std::vector<std::string> downloaded_urls;
	extern bool                     download_succeeds;
	extern int                      failing_fstat_attempt;

	void ResetDependencyAttempts();
	auto AttemptedDownloads() -> int;
	auto AttemptedOwnerUidLookups() -> int;
	auto FakeDownloadFile(const std::string                                           &url,
	                      howdy::native::download_models_internal::StagedDownloadFile &staged)
	    -> bool;
	auto
	SuccessfulFakeDownloadFile(const std::string                                           &url,
	                           howdy::native::download_models_internal::StagedDownloadFile &staged)
	    -> bool;
	auto FailingSha256File(int fd) -> std::optional<std::string>;
	auto SelectivelyFailingFstat(int fd, struct stat *stat_buf) -> int;
	auto TestModelFileOwnerUid() -> std::optional<uid_t>;

	struct EnvVarGuard {
		const char                *name;
		std::optional<std::string> previous;

		EnvVarGuard(const char *env_name, const std::string &value);
		EnvVarGuard(const EnvVarGuard &)                     = delete;
		auto operator=(const EnvVarGuard &) -> EnvVarGuard & = delete;
		~EnvVarGuard();
	};

	struct DownloadPaths {
		std::filesystem::path models_dir;
		std::filesystem::path output;
	};

	auto CaptureDownloadModelsStdout(
	    const std::filesystem::path &path, int *exit_code,
	    const howdy::native::download_models_internal::DownloadModelsDependencies &dependencies)
	    -> bool;
	auto RunTestDownload(const DownloadPaths &paths, int *exit_code,
	                     std::span<const howdy::native::OpenCvModelDescriptor>   models,
	                     howdy::native::download_models_internal::DownloadFileFn download_file,
	                     howdy::native::download_models_internal::Sha256FileFn   sha256_file =
	                         howdy::native::download_models_internal::Sha256FileDescriptor,
	                     howdy::native::download_models_internal::FstatFn fstat_file = ::fstat)
	    -> bool;
	auto RunFirstDownloadAttempt(const DownloadPaths &paths, int *exit_code) -> bool;

	auto RunDownloadModelsEntrypointTests() -> bool;
	auto RunAtomicFilesTests() -> bool;
	auto RunDownloadModelsIntegrityTests() -> bool;
	auto RunDownloadModelsManifestTests() -> bool;
	auto RunDownloadModelsProxyTests() -> bool;

}  // namespace howdy::test::download_models
