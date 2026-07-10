#include "cli/download_models_internal.hpp"

#include <array>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>

#include <sys/resource.h>
#include <sys/stat.h>

namespace {

	int download_attempts  = 0;
	int owner_uid_attempts = 0;

	void reset_dependency_attempts() {
		download_attempts  = 0;
		owner_uid_attempts = 0;
	}

	auto attempted_downloads() -> int {
		return download_attempts;
	}

	auto attempted_owner_uid_lookups() -> int {
		return owner_uid_attempts;
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
		++owner_uid_attempts;
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

	auto count_staged_files(const std::filesystem::path &directory, std::string_view prefix)
	    -> std::size_t {
		std::size_t count = 0;
		for (const auto &entry : std::filesystem::directory_iterator(directory)) {
			if (entry.path().filename().string().starts_with(prefix)) {
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

		[[nodiscard]] auto ok() const -> bool {
			return active;
		}
	};

	auto capture_download_models_stdout(
	    const std::filesystem::path &path, int *exit_code,
	    const howdy::native::download_models_internal::DownloadModelsDependencies &dependencies)
	    -> bool {
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
		        1, argv.data(), dependencies);
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
		reset_dependency_attempts();
		return capture_download_models_stdout(
		    output_path, exit_code,
		    howdy::native::download_models_internal::DownloadModelsDependencies{
		        .download_file        = fake_download_file,
		        .model_file_owner_uid = test_model_file_owner_uid,
		    });
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

	const auto null_download_models_dir = temp_root / "null-download-file" / "models";
	const auto null_download_output     = temp_root / "null-download-file-output.txt";
	ok &= expect(!fs::exists(null_download_models_dir, ec) && !ec,
	             "null download callback models directory starts absent");
	int null_download_exit = 0;
	{
		EnvVarGuard models_env("HOWDY_MODELS_DIR", null_download_models_dir.string());
		reset_dependency_attempts();
		ok &= expect(capture_download_models_stdout(
		                 null_download_output, &null_download_exit,
		                 howdy::native::download_models_internal::DownloadModelsDependencies{
		                     .download_file        = nullptr,
		                     .model_file_owner_uid = test_model_file_owner_uid,
		                 }),
		             "capture null download callback output");
	}
	ok &= expect(null_download_exit == EXIT_FAILURE, "null download callback aborts");
	ok &= expect(read_file(null_download_output).empty(), "null download callback emits no output");
	ok &= expect(attempted_downloads() == 0, "null download callback invokes no download");
	ok &= expect(attempted_owner_uid_lookups() == 0,
	             "null download callback invokes no owner lookup");
	ok &= expect(!fs::exists(null_download_models_dir, ec) && !ec,
	             "null download callback creates no models directory");

	const auto null_owner_models_dir = temp_root / "null-owner-uid" / "models";
	const auto null_owner_output     = temp_root / "null-owner-uid-output.txt";
	ok &= expect(!fs::exists(null_owner_models_dir, ec) && !ec,
	             "null owner callback models directory starts absent");
	int null_owner_exit = 0;
	{
		EnvVarGuard models_env("HOWDY_MODELS_DIR", null_owner_models_dir.string());
		reset_dependency_attempts();
		ok &= expect(capture_download_models_stdout(
		                 null_owner_output, &null_owner_exit,
		                 howdy::native::download_models_internal::DownloadModelsDependencies{
		                     .download_file        = fake_download_file,
		                     .model_file_owner_uid = nullptr,
		                 }),
		             "capture null owner callback output");
	}
	ok &= expect(null_owner_exit == EXIT_FAILURE, "null owner callback aborts");
	ok &= expect(read_file(null_owner_output).empty(), "null owner callback emits no output");
	ok &= expect(attempted_downloads() == 0, "null owner callback invokes no download");
	ok &= expect(attempted_owner_uid_lookups() == 0, "null owner callback invokes no owner lookup");
	ok &= expect(!fs::exists(null_owner_models_dir, ec) && !ec,
	             "null owner callback creates no models directory");

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
	ok &= expect(existing_exit == EXIT_FAILURE, "stubbed download aborts after readiness passes");
	ok &= expect(attempted_downloads() == 1,
	             "missing model in existing secure directory reaches download");
	ok &= expect(existing_stdout.contains("Downloading face_detection_yunet_2026may.onnx"),
	             "existing secure directory starts first model download");
	ok &= expect(count_staged_files(existing_models_dir, ".howdy-download-") == 0,
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
	ok &= expect(missing_parent_exit == EXIT_FAILURE,
	             "stubbed download aborts after missing parent creation");
	ok &= expect(fs::is_directory(missing_parent_models_dir, ec) && !ec,
	             "download-models creates missing models directory before readiness checks");
	ok &=
	    expect(attempted_downloads() == 1, "missing model after parent creation reaches download");
	ok &= expect(missing_parent_stdout.contains("Downloading face_detection_yunet_2026may.onnx"),
	             "missing parent path starts first model download");

	const auto blocked_models_dir = temp_root / "blocked-models";
	const auto blocked_output     = temp_root / "blocked-output.txt";
	ok &= expect(write_file(blocked_models_dir, "not a directory"),
	             "create file blocking models directory");

	int blocked_exit = 0;
	ok &= expect(run_first_download_attempt(blocked_models_dir, blocked_output, &blocked_exit),
	             "capture blocked models-dir download-models output");
	const auto blocked_stdout = read_file(blocked_output);
	ok &= expect(blocked_exit == EXIT_FAILURE, "blocked models directory aborts cleanly");
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
	ok &= expect(!howdy::native::sync_parent_directory(blocked_parent / "model.onnx"),
	             "parent-directory sync reports open failure");

	const auto overflow_destination = temp_root / "overflow-write.onnx";
	auto       overflow_staged =
	    howdy::native::prepare_staged_file(overflow_destination, ".howdy-download-");
	ok &= expect(overflow_staged.has_value(), "prepare staged file for overflow write callback");
	if (overflow_staged.has_value()) {
		howdy::native::download_models_internal::DownloadWriteContext write_context{
		    .staged = &*overflow_staged,
		};
		std::array<char, 2> payload        = {'x', 'y'};
		const auto          overflow_size  = (std::numeric_limits<std::size_t>::max() / 2) + 2;
		const auto          overflow_count = static_cast<std::size_t>(2);
		const auto result = howdy::native::download_models_internal::download_models_write_callback(
		    payload.data(), overflow_size, overflow_count, &write_context);
		ok &= expect(result == 0, "overflowing write callback returns 0");
		ok &= expect(read_file(overflow_staged->path).empty(),
		             "overflowing write callback leaves staged file empty");
		howdy::native::cleanup_staged_file(*overflow_staged);
	}

	const auto limited_destination = temp_root / "limited-write.onnx";
	auto       limited_staged =
	    howdy::native::prepare_staged_file(limited_destination, ".howdy-download-");
	ok &= expect(limited_staged.has_value(), "prepare staged file for cumulative write limit");
	if (limited_staged.has_value()) {
		howdy::native::download_models_internal::DownloadWriteContext write_context{
		    .staged    = &*limited_staged,
		    .max_bytes = 3,
		};
		std::array<char, 2> first_payload  = {'x', 'y'};
		std::array<char, 2> second_payload = {'z', '!'};
		const auto          first_result =
		    howdy::native::download_models_internal::download_models_write_callback(
		        first_payload.data(), 1, first_payload.size(), &write_context);
		const auto second_result =
		    howdy::native::download_models_internal::download_models_write_callback(
		        second_payload.data(), 1, second_payload.size(), &write_context);
		ok &= expect(first_result == first_payload.size(),
		             "write callback accepts data within cumulative limit");
		ok &= expect(second_result == 0, "write callback rejects data exceeding cumulative limit");
		ok &= expect(write_context.bytes_written == first_payload.size(),
		             "write callback tracks only committed bytes");
		ok &= expect(read_file(limited_staged->path) == "xy",
		             "write callback leaves rejected chunk unwritten");
		howdy::native::cleanup_staged_file(*limited_staged);
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

	const auto atomic_new_path = temp_root / "atomic" / "new-file";
	ok &=
	    expect(howdy::native::write_atomic_file(atomic_new_path, "new content", S_IRUSR | S_IWUSR),
	           "atomic writer creates new file");
	ok &=
	    expect(read_file(atomic_new_path) == "new content", "atomic writer preserves new content");
	struct stat atomic_new_stat{};
	ok &=
	    expect(stat(atomic_new_path.c_str(), &atomic_new_stat) == 0, "stat atomic writer new file");
	ok &= expect(S_ISREG(atomic_new_stat.st_mode), "atomic writer new target is regular file");
	ok &= expect((atomic_new_stat.st_mode & 07777) == 0600,
	             "atomic writer applies requested new-file mode");
	ok &= expect(count_staged_files(atomic_new_path.parent_path(), ".howdy-atomic-") == 0,
	             "atomic writer removes staged file after new-file install");

	const auto atomic_existing_path = temp_root / "atomic-existing";
	ok &=
	    expect(write_file(atomic_existing_path, "initial"), "create existing atomic writer target");
	ok &= expect(chmod(atomic_existing_path.c_str(), 0640) == 0,
	             "set existing atomic writer target mode");
	ok &= expect(howdy::native::write_atomic_file(atomic_existing_path, "replacement", 0600),
	             "atomic writer replaces existing file");
	ok &= expect(read_file(atomic_existing_path) == "replacement",
	             "atomic writer preserves replacement content");
	struct stat atomic_existing_stat{};
	ok &= expect(stat(atomic_existing_path.c_str(), &atomic_existing_stat) == 0,
	             "stat replaced atomic writer file");
	ok &= expect((atomic_existing_stat.st_mode & 07777) == 0640,
	             "atomic writer preserves existing-file mode");
	ok &= expect(count_staged_files(atomic_existing_path.parent_path(), ".howdy-atomic-") == 0,
	             "atomic writer removes staged file after replacement");

	const auto default_mode_path = temp_root / "default-mode-existing";
	ok &= expect(write_file(default_mode_path, "old"), "create default-mode policy target");
	ok &=
	    expect(chmod(default_mode_path.c_str(), 0600) == 0, "set default-mode policy target mode");
	auto default_mode_staged = howdy::native::prepare_staged_file(
	    default_mode_path, ".howdy-default-mode-", S_IRUSR | S_IWUSR | S_IRGRP,
	    howdy::native::StagedFileMetadataPolicy::kUseDefaultMode);
	ok &= expect(default_mode_staged.has_value(), "prepare default-mode policy staged file");
	if (default_mode_staged.has_value()) {
		ok &= expect(howdy::native::write_all_to_fd(default_mode_staged->fd.get(), "new"),
		             "write default-mode policy staged file");
		ok &= expect(howdy::native::install_staged_file(*default_mode_staged, default_mode_path),
		             "install default-mode policy staged file");
		ok &= expect(read_file(default_mode_path) == "new",
		             "default-mode policy installs replacement content");
		struct stat default_mode_stat{};
		ok &= expect(stat(default_mode_path.c_str(), &default_mode_stat) == 0,
		             "stat default-mode policy target");
		ok &= expect((default_mode_stat.st_mode & 07777) == 0640,
		             "default-mode policy applies requested mode to existing file");
		ok &=
		    expect(count_staged_files(default_mode_path.parent_path(), ".howdy-default-mode-") == 0,
		           "default-mode policy removes staged file after replacement");
	}

	const auto atomic_directory_target = temp_root / "atomic-directory-target";
	fs::create_directory(atomic_directory_target, ec);
	ok &= expect(!ec, "create directory atomic writer target");
	ok &= expect(!howdy::native::write_atomic_file(atomic_directory_target, "must not replace"),
	             "atomic writer rejects directory target");
	ok &= expect(fs::is_directory(atomic_directory_target, ec) && !ec,
	             "atomic writer leaves directory target unchanged");
	ok &= expect(count_staged_files(atomic_directory_target.parent_path(), ".howdy-atomic-") == 0,
	             "atomic writer creates no staged file for directory target");

	const auto atomic_blocked_parent = temp_root / "atomic-blocked-parent";
	ok &= expect(write_file(atomic_blocked_parent, "blocking content"),
	             "create file blocking atomic writer parent");
	const auto atomic_blocked_path = atomic_blocked_parent / "child" / "file";
	ok &= expect(!howdy::native::write_atomic_file(atomic_blocked_path, "must not write"),
	             "atomic writer rejects blocked parent path");
	ok &= expect(!fs::exists(atomic_blocked_parent / "child", ec) && !ec,
	             "atomic writer creates no child under blocked parent");
	ok &= expect(read_file(atomic_blocked_parent) == "blocking content",
	             "atomic writer preserves blocking file content");

	const auto atomic_write_failure_path = temp_root / "atomic-write-failure";
	ok &= expect(write_file(atomic_write_failure_path, "original content"),
	             "create atomic writer write-failure target");
	struct rlimit original_file_size_limit{};
	const bool    have_file_size_limit = getrlimit(RLIMIT_FSIZE, &original_file_size_limit) == 0;
	ok &= expect(have_file_size_limit, "read file-size resource limit");
	const auto previous_sigxfsz_handler = std::signal(SIGXFSZ, SIG_IGN);
	const bool have_sigxfsz_handler     = previous_sigxfsz_handler != SIG_ERR;
	ok &= expect(have_sigxfsz_handler, "ignore file-size signal for failed atomic write");
	bool atomic_write_failed = false;
	if (have_file_size_limit && have_sigxfsz_handler) {
		auto zero_file_size_limit     = original_file_size_limit;
		zero_file_size_limit.rlim_cur = 0;
		const bool limited_file_size  = setrlimit(RLIMIT_FSIZE, &zero_file_size_limit) == 0;
		ok &= expect(limited_file_size, "limit file size for failed atomic write");
		if (limited_file_size) {
			atomic_write_failed =
			    !howdy::native::write_atomic_file(atomic_write_failure_path, "replacement");
			ok &= expect(setrlimit(RLIMIT_FSIZE, &original_file_size_limit) == 0,
			             "restore file-size resource limit");
		}
	}
	if (have_sigxfsz_handler) {
		ok &= expect(std::signal(SIGXFSZ, previous_sigxfsz_handler) != SIG_ERR,
		             "restore file-size signal handler");
	}
	ok &= expect(atomic_write_failed, "atomic writer reports staged content write failure");
	ok &= expect(read_file(atomic_write_failure_path) == "original content",
	             "failed atomic write preserves existing target");
	ok &= expect(count_staged_files(atomic_write_failure_path.parent_path(), ".howdy-atomic-") == 0,
	             "failed atomic write removes staged file");

	fs::remove_all(temp_root, ec);
	return ok ? 0 : 1;
}
