#include "cli/download_models_test_support.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>

namespace howdy::test::download_models {

	int                      download_attempts     = 0;
	int                      owner_uid_attempts    = 0;
	int                      fstat_attempts        = 0;
	int                      failing_fstat_attempt = 0;
	std::string              downloaded_content;
	std::vector<std::string> downloaded_urls;
	bool                     download_succeeds = false;

	void ResetDependencyAttempts() {
		download_attempts  = 0;
		owner_uid_attempts = 0;
		fstat_attempts     = 0;
		downloaded_urls.clear();
	}

	auto AttemptedDownloads() -> int {
		return download_attempts;
	}

	auto AttemptedOwnerUidLookups() -> int {
		return owner_uid_attempts;
	}

	auto FakeDownloadFile(const std::string                                           &url,
	                      howdy::native::download_models_internal::StagedDownloadFile &staged)
	    -> bool {
		(void)url;
		(void)staged;
		++download_attempts;
		return false;
	}

	auto
	SuccessfulFakeDownloadFile(const std::string                                           &url,
	                           howdy::native::download_models_internal::StagedDownloadFile &staged)
	    -> bool {
		++download_attempts;
		downloaded_urls.push_back(url);
		return download_succeeds &&
		       howdy::native::WriteAllToFd(staged.fd.Get(), downloaded_content);
	}

	auto FailingSha256File(int /*fd*/) -> std::optional<std::string> {
		return std::nullopt;
	}

	auto SelectivelyFailingFstat(const int fd, struct stat *stat_buf) -> int {
		++fstat_attempts;
		if (fstat_attempts == failing_fstat_attempt) {
			errno = EIO;
			return -1;
		}
		return fstat(fd, stat_buf);
	}

	auto TestModelFileOwnerUid() -> std::optional<uid_t> {
		++owner_uid_attempts;
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
	    : name(env_name)
	    , previous(GetEnvValue(env_name)) {
		setenv(name, value.c_str(), 1);
	}

	EnvVarGuard::~EnvVarGuard() {
		if (previous.has_value()) {
			setenv(name, previous->c_str(), 1);
			return;
		}
		unsetenv(name);
	}

	namespace {

		struct StdoutRedirectGuard {
			int  saved_stdout = -1;
			bool active       = false;

			explicit StdoutRedirectGuard(const std::filesystem::path &path)
			    : saved_stdout(dup(STDOUT_FILENO)) {
				if (saved_stdout < 0) {
					return;
				}

				const int output_fd =
				    open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
				if (output_fd < 0) {
					close(saved_stdout);
					saved_stdout = -1;
					return;
				}

				if (dup2(output_fd, STDOUT_FILENO) < 0) {
					close(output_fd);
					close(saved_stdout);
					saved_stdout = -1;
					return;
				}
				close(output_fd);
				active = true;
			}

			StdoutRedirectGuard(const StdoutRedirectGuard &)                     = delete;
			auto operator=(const StdoutRedirectGuard &) -> StdoutRedirectGuard & = delete;

			~StdoutRedirectGuard() {
				if (saved_stdout >= 0) {
					std::cout.flush();
					dup2(saved_stdout, STDOUT_FILENO);
					close(saved_stdout);
				}
			}

			[[nodiscard]] auto Ok() const -> bool {
				return active;
			}
		};

	}  // namespace

	auto CaptureDownloadModelsStdout(
	    const std::filesystem::path &path, int *exit_code,
	    const howdy::native::download_models_internal::DownloadModelsDependencies &dependencies)
	    -> bool {
		StdoutRedirectGuard stdout_redirect(path);
		if (!stdout_redirect.Ok()) {
			return false;
		}

		std::array<char *, 2> argv = {
		    const_cast<char *>("howdy-download-models"),
		    nullptr,
		};
		*exit_code = howdy::native::download_models_internal::DownloadModelsMainWithDependencies(
		    1, argv.data(), dependencies);
		return true;
	}

	auto
	RunTestDownload(const DownloadPaths &paths, int *exit_code,
	                const std::span<const howdy::native::OpenCvModelDescriptor>   models,
	                const howdy::native::download_models_internal::DownloadFileFn download_file,
	                const howdy::native::download_models_internal::Sha256FileFn   sha256_file,
	                const howdy::native::download_models_internal::FstatFn fstat_file) -> bool {
		EnvVarGuard models_env("HOWDY_MODELS_DIR", paths.models_dir.string());
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
		EnvVarGuard models_env("HOWDY_MODELS_DIR", paths.models_dir.string());
		ResetDependencyAttempts();
		return CaptureDownloadModelsStdout(
		    paths.output, exit_code,
		    howdy::native::download_models_internal::DownloadModelsDependencies{
		        .download_file        = FakeDownloadFile,
		        .model_file_owner_uid = TestModelFileOwnerUid,
		    });
	}

}  // namespace howdy::test::download_models
