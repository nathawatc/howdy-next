#pragma once

#include "cli/download_models_internal.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace howdy::test::download_models {

	using howdy::test::count_files_with_prefix;
	using howdy::test::read_file;
	using howdy::test::write_file;

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

	void reset_dependency_attempts();
	auto attempted_downloads() -> int;
	auto attempted_owner_uid_lookups() -> int;
	auto fake_download_file(const std::string                                           &url,
	                        howdy::native::download_models_internal::StagedDownloadFile &staged)
	    -> bool;
	auto successful_fake_download_file(
	    const std::string &url, howdy::native::download_models_internal::StagedDownloadFile &staged)
	    -> bool;
	auto failing_sha256_file(int fd) -> std::optional<std::string>;
	auto selectively_failing_fstat(int fd, struct stat *stat_buf) -> int;
	auto test_model_file_owner_uid() -> std::optional<uid_t>;

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

	auto capture_download_models_stdout(
	    const std::filesystem::path &path, int *exit_code,
	    const howdy::native::download_models_internal::DownloadModelsDependencies &dependencies)
	    -> bool;
	auto run_test_download(const DownloadPaths &paths, int *exit_code,
	                       std::span<const howdy::native::OpenCvModelDescriptor>   models,
	                       howdy::native::download_models_internal::DownloadFileFn download_file,
	                       howdy::native::download_models_internal::Sha256FileFn   sha256_file =
	                           howdy::native::download_models_internal::sha256_file_descriptor,
	                       howdy::native::download_models_internal::FstatFn fstat_file = ::fstat)
	    -> bool;
	auto run_first_download_attempt(const DownloadPaths &paths, int *exit_code) -> bool;

	auto run_download_models_entrypoint_tests() -> bool;
	auto run_atomic_files_tests() -> bool;
	auto run_download_models_integrity_tests() -> bool;
	auto run_download_models_manifest_tests() -> bool;
	auto run_download_models_proxy_tests() -> bool;

}  // namespace howdy::test::download_models
