#include "cli/download_models_internal.hpp"

#include <array>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
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

	auto write_file(const std::filesystem::path &path, const std::string &content) -> bool {
		std::ofstream out(path);
		if (!out.is_open()) {
			return false;
		}
		out << content;
		return out.good();
	}

	auto count_staged_downloads(const std::filesystem::path &directory) -> std::size_t {
		std::size_t count = 0;
		for (const auto &entry : std::filesystem::directory_iterator(directory)) {
			if (entry.path().filename().string().starts_with(".howdy-download-")) {
				++count;
			}
		}
		return count;
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
	ok &= expect(count_staged_downloads(existing_models_dir) == 0,
	             "failed download removes staged file");

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

	const auto blocked_models_dir = temp_root / "blocked-models";
	const auto blocked_output     = temp_root / "blocked-output.txt";
	ok &= expect(write_file(blocked_models_dir, "not a directory"),
	             "create file blocking models directory");

	int blocked_exit = 0;
	ok &= expect(run_first_download_attempt(blocked_models_dir, blocked_output, &blocked_exit),
	             "capture blocked models-dir download-models output");
	const auto blocked_stdout = read_file(blocked_output);
	ok &= expect(blocked_exit == 1, "blocked models directory aborts cleanly");
	ok &=
	    expect(attempted_downloads() == 0, "blocked models directory stops before first download");
	ok &= expect(blocked_stdout.contains("Failed to create models directory:"),
	             "blocked models directory reports setup failure");

	howdy::native::StagedFile empty_path_install{
	    .fd   = howdy::native::ScopedFd(open("/dev/null", O_WRONLY)),
	    .path = {},
	};
	ok &= expect(empty_path_install.fd.get() >= 0, "open fd for empty-path staged install");
	ok &= expect(!howdy::native::install_staged_file(empty_path_install, temp_root / "unused"),
	             "staged install rejects empty path");
	ok &= expect(empty_path_install.fd.get() < 0, "empty-path staged install closes fd");

	const auto blocked_parent = temp_root / "blocked-parent";
	ok &= expect(write_file(blocked_parent, "not a directory"),
	             "create file blocking staged parent directory");
	ok &= expect(!howdy::native::prepare_staged_file(blocked_parent / "models" / "model.onnx",
	                                                 ".howdy-download-")
	                  .has_value(),
	             "prepare staged file returns nullopt when parent creation fails");

	const auto overflow_destination = temp_root / "overflow-write.onnx";
	auto       overflow_staged =
	    howdy::native::prepare_staged_file(overflow_destination, ".howdy-download-");
	ok &= expect(overflow_staged.has_value(), "prepare staged file for overflow write callback");
	if (overflow_staged.has_value()) {
		std::array<char, 2> payload        = {'x', 'y'};
		const auto          overflow_size  = (std::numeric_limits<std::size_t>::max() / 2) + 2;
		const auto          overflow_count = static_cast<std::size_t>(2);
		const auto result = howdy::native::download_models_internal::download_models_write_callback(
		    payload.data(), overflow_size, overflow_count, &*overflow_staged);
		ok &= expect(result == 0, "overflowing write callback returns 0");
		ok &= expect(read_file(overflow_staged->path).empty(),
		             "overflowing write callback leaves staged file empty");
		howdy::native::cleanup_staged_file(*overflow_staged);
	}

	const auto successful_install_destination = temp_root / "successful-install.onnx";
	auto       successful_install =
	    howdy::native::prepare_staged_file(successful_install_destination, ".howdy-download-");
	ok &= expect(successful_install.has_value(), "prepare staged file for successful install");
	if (successful_install.has_value()) {
		const auto staged_path = successful_install->path;
		ok &= expect(howdy::native::write_all_to_fd(successful_install->fd.get(), "model"),
		             "download-like write leaves staged fd open before install");
		ok &= expect(successful_install->fd.get() >= 0,
		             "download-like staged fd remains open until install");
		ok &= expect(
		    howdy::native::install_staged_file(*successful_install, successful_install_destination),
		    "install staged file fsyncs and closes download-like open fd");
		ok &= expect(successful_install->fd.get() < 0, "successful install closes staged fd");
		ok &= expect(successful_install->path.empty(), "successful install clears staged path");
		ok &= expect(fs::exists(successful_install_destination, ec) && !ec,
		             "successful install creates destination");
		ok &= expect(read_file(successful_install_destination) == "model",
		             "successful install preserves staged content");
		ok &= expect(!fs::exists(staged_path, ec) && !ec,
		             "successful install removes staged temp path");
	}

	const auto failed_install_destination = temp_root / "failed-install.onnx";
	auto       failed_install =
	    howdy::native::prepare_staged_file(failed_install_destination, ".howdy-download-");
	ok &= expect(failed_install.has_value(), "prepare staged file for failed install");
	if (failed_install.has_value()) {
		const auto staged_path = failed_install->path;
		ok &= expect(
		    !howdy::native::install_staged_file(*failed_install, temp_root / "missing" / "model"),
		    "staged install fails for missing destination parent");
		ok &= expect(!fs::exists(staged_path, ec) && !ec,
		             "failed staged install removes staged file");
	}

	fs::remove_all(temp_root, ec);
	return ok ? 0 : 1;
}
