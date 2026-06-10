#include "cli/download_models_internal.hpp"

#include <array>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <unistd.h>

#include <sys/stat.h>

namespace {

	int download_attempts = 0;

	void reset_download_attempts() {
		download_attempts = 0;
	}

	auto attempted_downloads() -> int {
		return download_attempts;
	}

	auto fake_download_file(const std::string                                           &url,
	                        howdy::native::download_models_internal::StagedDownloadFile &staged)
	    -> bool {
		(void)url;
		(void)staged;
		++download_attempts;
		return false;
	}

	auto test_model_file_owner_uid() -> std::optional<uid_t> {
		return std::nullopt;
	}

	auto read_file(const std::filesystem::path &path) -> std::string {
		std::ifstream in(path);
		if (!in.is_open()) {
			return {};
		}
		return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
	}

	auto get_env_value(const char *name) -> std::optional<std::string> {
		const char *value = std::getenv(name);
		if (value == nullptr) {
			return std::nullopt;
		}
		return std::string(value);
	}

	struct EnvVarGuard {
		const char                *name;
		std::optional<std::string> previous;

		EnvVarGuard(const char *env_name, const std::string &value)
		    : name(env_name)
		    , previous(get_env_value(env_name)) {
			setenv(name, value.c_str(), 1);
		}

		EnvVarGuard(const EnvVarGuard &)                     = delete;
		auto operator=(const EnvVarGuard &) -> EnvVarGuard & = delete;

		~EnvVarGuard() {
			if (previous.has_value()) {
				setenv(name, previous->c_str(), 1);
				return;
			}
			unsetenv(name);
		}
	};

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

	struct StdoutRedirectGuard {
		int  saved_stdout = -1;
		bool active       = false;

		explicit StdoutRedirectGuard(const std::filesystem::path &path) {
			saved_stdout = dup(STDOUT_FILENO);
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

		[[nodiscard]] auto ok() const -> bool {
			return active;
		}
	};

	auto capture_download_models_stdout(const std::filesystem::path &path, int *exit_code) -> bool {
		StdoutRedirectGuard stdout_redirect(path);
		if (!stdout_redirect.ok()) {
			return false;
		}

		std::array<char *, 2> argv = {
		    const_cast<char *>("howdy-download-models"),
		    nullptr,
		};
		*exit_code =
		    howdy::native::download_models_internal::download_models_main_with_dependencies(
		        1, argv.data(),
		        howdy::native::download_models_internal::DownloadModelsDependencies{
		            .download_file        = fake_download_file,
		            .model_file_owner_uid = test_model_file_owner_uid,
		        });
		return true;
	}

	auto expect(bool condition, const std::string &message) -> bool {
		if (!condition) {
			std::cerr << "FAIL: " << message << "\n";
			return false;
		}
		return true;
	}

	auto run_first_download_attempt(const std::filesystem::path &models_dir,
	                                const std::filesystem::path &output_path, int *exit_code)
	    -> bool {
		EnvVarGuard models_env("HOWDY_MODELS_DIR", models_dir.string());
		reset_download_attempts();
		return capture_download_models_stdout(output_path, exit_code);
	}

}  // namespace

auto main() -> int {
	namespace fs = std::filesystem;

	bool             ok = true;
	const UmaskGuard umask_guard(0022);
	const auto       temp_root = fs::current_path() / "howdy-download-models-test";
	std::error_code  ec;
	fs::remove_all(temp_root, ec);
	fs::create_directories(temp_root, ec);
	ok &= expect(!ec, "create temp root");

	const auto existing_models_dir = temp_root / "existing-models";
	const auto existing_output     = temp_root / "existing-output.txt";
	fs::create_directories(existing_models_dir, ec);
	ok &= expect(!ec, "create existing models directory");
	ok &= expect(chmod(existing_models_dir.c_str(), 0755) == 0,
	             "make existing models directory secure");

	int existing_exit = 0;
	ok &= expect(run_first_download_attempt(existing_models_dir, existing_output, &existing_exit),
	             "capture existing-directory download-models output");
	const auto existing_stdout = read_file(existing_output);
	ok &= expect(existing_exit == 1, "stubbed download aborts after readiness passes");
	ok &= expect(attempted_downloads() == 1,
	             "missing model in existing secure directory reaches download");
	ok &= expect(existing_stdout.contains("Downloading face_detection_yunet_2023mar_int8bq.onnx"),
	             "existing secure directory starts first model download");

	const auto missing_parent_models_dir = temp_root / "missing-parent" / "models";
	const auto missing_parent_output     = temp_root / "missing-parent-output.txt";
	ok &= expect(!fs::exists(missing_parent_models_dir, ec) && !ec,
	             "missing parent models directory starts absent");

	int missing_parent_exit = 0;
	ok &= expect(run_first_download_attempt(missing_parent_models_dir, missing_parent_output,
	                                        &missing_parent_exit),
	             "capture missing-parent download-models output");
	const auto missing_parent_stdout = read_file(missing_parent_output);
	ok &= expect(missing_parent_exit == 1, "stubbed download aborts after missing parent creation");
	ok &= expect(fs::is_directory(missing_parent_models_dir, ec) && !ec,
	             "download-models creates missing models directory before readiness checks");
	ok &=
	    expect(attempted_downloads() == 1, "missing model after parent creation reaches download");
	ok &= expect(
	    missing_parent_stdout.contains("Downloading face_detection_yunet_2023mar_int8bq.onnx"),
	    "missing parent path starts first model download");

	fs::remove_all(temp_root, ec);
	return ok ? 0 : 1;
}
