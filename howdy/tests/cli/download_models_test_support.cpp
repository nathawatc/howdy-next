#include "cli/download_models_test_support.hpp"

#include "cli/download_models/internal.hpp"
#include "model_assets/opencv_model_manifest.hpp"
#include "support/fd_io.hpp"

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>

namespace howdy::test::download_models {

	auto DependenciesState() -> DependencyState & {
		static DependencyState state;
		return state;
	}

	void ResetDependencyAttempts() {
		auto &state              = DependenciesState();
		state.download_attempts  = 0;
		state.owner_uid_attempts = 0;
		state.fstat_attempts     = 0;
		state.downloaded_urls.clear();
	}

	auto AttemptedDownloads() -> int {
		return DependenciesState().download_attempts;
	}

	auto AttemptedOwnerUidLookups() -> int {
		return DependenciesState().owner_uid_attempts;
	}

	auto FakeDownloadFile(const std::string                                           &url,
	                      howdy::native::download_models_internal::StagedDownloadFile &staged)
	    -> bool {
		(void)url;
		(void)staged;
		++DependenciesState().download_attempts;
		return false;
	}

	auto
	SuccessfulFakeDownloadFile(const std::string                                           &url,
	                           howdy::native::download_models_internal::StagedDownloadFile &staged)
	    -> bool {
		++DependenciesState().download_attempts;
		auto &state = DependenciesState();
		state.downloaded_urls.push_back(url);
		return state.download_succeeds &&
		       howdy::native::WriteAllToFd(staged.fd.Get(), state.downloaded_content);
	}

	auto FailingSha256File(int /*fd*/) -> std::optional<std::string> {
		return std::nullopt;
	}

	auto SelectivelyFailingFstat(const int fd, struct stat *stat_buf) -> int {
		auto &state = DependenciesState();
		++state.fstat_attempts;
		if (state.fstat_attempts == state.failing_fstat_attempt) {
			errno = EIO;
			return -1;
		}
		return fstat(fd, stat_buf);
	}

	auto TestModelFileOwnerUid() -> std::optional<uid_t> {
		++DependenciesState().owner_uid_attempts;
		return std::nullopt;
	}

	namespace {

		auto GetEnvValue(const char *name) -> std::optional<std::string> {
			const char *value = std::getenv(name);
			if (value == nullptr) {
				return std::nullopt;
			}
			return std::string(value);
		}

	}  // namespace

	EnvVarGuard::EnvVarGuard(const char *env_name, const std::string &value)
	    : name_(env_name)
	    , previous_(GetEnvValue(env_name)) {
		setenv(name_, value.c_str(), 1);
	}

	EnvVarGuard::~EnvVarGuard() {
		if (previous_.has_value()) {
			setenv(name_, previous_->c_str(), 1);
			return;
		}
		unsetenv(name_);
	}

	namespace {

		class StdoutRedirectGuard {
		public:
			StdoutRedirectGuard(StdoutRedirectGuard &&)                     = delete;
			auto operator=(StdoutRedirectGuard &&) -> StdoutRedirectGuard & = delete;

			explicit StdoutRedirectGuard(const std::filesystem::path &path)
			    : saved_stdout_(fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 0)) {
				if (saved_stdout_ < 0) {
					return;
				}

				const int output_fd =
				    open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, S_IRUSR | S_IWUSR);
				if (output_fd < 0) {
					close(saved_stdout_);
					saved_stdout_ = -1;
					return;
				}

				if (dup2(output_fd, STDOUT_FILENO) < 0) {
					close(output_fd);
					close(saved_stdout_);
					saved_stdout_ = -1;
					return;
				}
				close(output_fd);
				active_ = true;
			}

			StdoutRedirectGuard(const StdoutRedirectGuard &)                     = delete;
			auto operator=(const StdoutRedirectGuard &) -> StdoutRedirectGuard & = delete;

			~StdoutRedirectGuard() {
				if (saved_stdout_ >= 0) {
					std::cout.flush();
					dup2(saved_stdout_, STDOUT_FILENO);
					close(saved_stdout_);
				}
			}

			[[nodiscard]] auto Ok() const -> bool {
				return active_;
			}

		private:
			int  saved_stdout_ = -1;
			bool active_       = false;
		};

	}  // namespace

	auto CaptureDownloadModelsStdout(
	    const std::filesystem::path &path, int *exit_code,
	    const howdy::native::download_models_internal::DownloadModelsDependencies &dependencies)
	    -> bool {
		StdoutRedirectGuard const stdout_redirect(path);
		if (!stdout_redirect.Ok()) {
			return false;
		}

		auto fixture_dependencies            = dependencies;
		fixture_dependencies.validation_root = {std::filesystem::current_path() /
		                                        "howdy-download-models-test"};
		*exit_code = howdy::native::download_models_internal::DownloadModelsMainWithDependencies(
		    {}, fixture_dependencies);
		return true;
	}

	auto
	RunTestDownload(const DownloadPaths &paths, int *exit_code,
	                const std::span<const howdy::native::OpenCvModelDescriptor>   models,
	                const howdy::native::download_models_internal::DownloadFileFn download_file,
	                const howdy::native::download_models_internal::Sha256FileFn   sha256_file,
	                const howdy::native::download_models_internal::FstatFn fstat_file) -> bool {
		EnvVarGuard const models_env("HOWDY_MODELS_DIR", paths.models_dir.string());
		ResetDependencyAttempts();
		return CaptureDownloadModelsStdout(
		    paths.output, exit_code,
		    howdy::native::download_models_internal::DownloadModelsDependencies{
		        .download_file        = download_file,
		        .model_file_owner_uid = TestModelFileOwnerUid,
		        .sha256_file          = sha256_file,
		        .fstat_file           = fstat_file,
		        .models               = models,
		    });
	}

	auto RunFirstDownloadAttempt(const DownloadPaths &paths, int *exit_code) -> bool {
		EnvVarGuard const models_env("HOWDY_MODELS_DIR", paths.models_dir.string());
		ResetDependencyAttempts();
		return CaptureDownloadModelsStdout(
		    paths.output, exit_code,
		    howdy::native::download_models_internal::DownloadModelsDependencies{
		        .download_file        = FakeDownloadFile,
		        .model_file_owner_uid = TestModelFileOwnerUid,
		    });
	}

}  // namespace howdy::test::download_models
