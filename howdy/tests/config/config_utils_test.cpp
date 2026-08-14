#include "config/config_limits.hpp"
#include "config/config_utils.hpp"
#include "config/config_validation.hpp"
#include "test_support.hpp"

#include <cerrno>
#include <clocale>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

#include <sys/resource.h>
#include <sys/stat.h>

namespace {

	using howdy::test::expect;

	auto write_file(const std::filesystem::path &path, const std::string &content) -> bool {
		std::ofstream out(path);
		if (!out.is_open()) {
			return false;
		}
		out << content;
		return out.good();
	}

	auto read_file(const std::filesystem::path &path) -> std::string {
		std::ifstream in(path);
		if (!in.is_open()) {
			return {};
		}
		return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
	}

	auto count_staged_configs(const std::filesystem::path &directory) -> std::size_t {
		std::size_t count = 0;
		for (const auto &entry : std::filesystem::directory_iterator(directory)) {
			if (entry.path().filename().string().starts_with(".howdy-config-")) {
				++count;
			}
		}
		return count;
	}

	auto fail_parent_sync(const std::filesystem::path & /*path*/) -> bool {
		return false;
	}

	auto lock_path_for_config(const std::filesystem::path &config_path) -> std::filesystem::path {
		return config_path.string() + ".lock";
	}

	auto get_env_value(const char *name) -> std::optional<std::string> {
		const char *value = std::getenv(name);
		if (value == nullptr) {
			return std::nullopt;
		}
		return std::string(value);
	}

	void restore_env_value(const char *name, const std::optional<std::string> &value) {
		if (value.has_value()) {
			setenv(name, value->c_str(), 1);
			return;
		}
		unsetenv(name);
	}

	auto expect_float_write_paths(const std::filesystem::path &config_path) -> bool {
		auto write_float_config = [&]() -> bool {
			return write_file(config_path, "[video]\n"
			                               "clahe_clip_limit = 2\n"
			                               "\n"
			                               "[face]\n"
			                               "yunet_score_threshold = 0.9\n"
			                               "yunet_nms_threshold = 0.4\n"
			                               "sface_threshold = 0.5\n");
		};

		auto expect_float_write_path = [&](const std::string &label) -> bool {
			bool ok = true;
			ok &= expect(write_float_config(), label + ": write baseline float config");
			ok &=
			    expect(howdy::native::update_config_value(config_path, "clahe_clip_limit", "1.25"),
			           label + ": update_config_value accepts clahe_clip_limit dot decimal");
			ok &= expect(read_file(config_path).contains("clahe_clip_limit = 1.25\n"),
			             label + ": clahe_clip_limit written unchanged");
			ok &= expect(
			    howdy::native::update_config_value(config_path, "yunet_score_threshold", "0.8845"),
			    label + ": update_config_value accepts yunet_score_threshold dot decimal");
			ok &= expect(read_file(config_path).contains("yunet_score_threshold = 0.8845\n"),
			             label + ": yunet_score_threshold written unchanged");
			ok &= expect(
			    howdy::native::update_config_value(config_path, "yunet_nms_threshold", "0.3"),
			    label + ": update_config_value accepts yunet_nms_threshold dot decimal");
			ok &= expect(read_file(config_path).contains("yunet_nms_threshold = 0.3\n"),
			             label + ": yunet_nms_threshold written unchanged");
			ok &=
			    expect(howdy::native::update_config_value(config_path, "sface_threshold", "0.6942"),
			           label + ": update_config_value accepts sface_threshold dot decimal");
			ok &= expect(read_file(config_path).contains("sface_threshold = 0.6942\n"),
			             label + ": sface_threshold written unchanged");

			for (const auto *const value : {"1,25", "1.25abc", "nan", "inf", "+inf", "-inf"}) {
				const auto before_invalid = read_file(config_path);
				ok &= expect(
				    !howdy::native::update_config_value(config_path, "clahe_clip_limit", value),
				    label + ": update_config_value rejects invalid float " + std::string(value));
				ok &= expect(read_file(config_path) == before_invalid,
				             label + ": invalid float update leaves config unchanged for " +
				                 std::string(value));
			}
			return ok;
		};

		bool ok = expect_float_write_path("C locale");

		const char *current_locale = std::setlocale(LC_ALL, nullptr);
		const auto  previous_locale =
		    current_locale == nullptr ? std::optional<std::string>() : std::string(current_locale);
		const auto previous_lc_all     = get_env_value("LC_ALL");
		const auto previous_lc_numeric = get_env_value("LC_NUMERIC");
		const auto previous_lang       = get_env_value("LANG");
		unsetenv("LC_ALL");
		setenv("LANG", "C", 1);
		setenv("LC_NUMERIC", "nl_NL.UTF-8", 1);
		if (std::setlocale(LC_ALL, "") != nullptr) {
			ok &= expect_float_write_path("LC_NUMERIC=nl_NL.UTF-8");
		} else {
			std::cerr << "SKIP: nl_NL.UTF-8 locale is not generated\n";
		}
		restore_env_value("LC_ALL", previous_lc_all);
		restore_env_value("LC_NUMERIC", previous_lc_numeric);
		restore_env_value("LANG", previous_lang);
		if (previous_locale.has_value()) {
			std::setlocale(LC_ALL, previous_locale->c_str());
		}
		return ok;
	}

	struct FileSizeLimitGuard {
		using SignalHandler = void (*)(int);

		rlimit        original{};
		bool          have_original    = false;
		bool          limit_changed    = false;
		SignalHandler previous_sigxfsz = SIG_DFL;
		bool          signal_changed   = false;

		FileSizeLimitGuard()
		    : have_original(getrlimit(RLIMIT_FSIZE, &original) == 0) {
			if (have_original) {
				previous_sigxfsz = std::signal(SIGXFSZ, SIG_IGN);
				signal_changed   = previous_sigxfsz != SIG_ERR;
			}
		}

		[[nodiscard]] auto ready() const -> bool {
			return have_original && signal_changed;
		}

		auto set_zero() -> bool {
			if (!ready()) {
				return false;
			}

			auto zero_limit     = original;
			zero_limit.rlim_cur = 0;
			if (setrlimit(RLIMIT_FSIZE, &zero_limit) != 0) {
				return false;
			}

			limit_changed = true;
			return true;
		}

		auto restore() -> bool {
			bool ok = true;
			if (limit_changed) {
				ok            = setrlimit(RLIMIT_FSIZE, &original) == 0;
				limit_changed = false;
			}
			if (signal_changed) {
				ok             = std::signal(SIGXFSZ, previous_sigxfsz) != SIG_ERR && ok;
				signal_changed = false;
			}
			return ok;
		}

		~FileSizeLimitGuard() {
			(void)restore();
		}
	};

}  // namespace

auto main() -> int {
	namespace fs = std::filesystem;

	bool            ok        = true;
	const auto      temp_root = fs::current_path() / "howdy-config-utils-test-work";
	std::error_code ec;
	fs::remove_all(temp_root, ec);
	fs::create_directories(temp_root, ec);
	ok &= expect(!ec, "create temp root");

	const auto config_path = temp_root / "config.ini";
	ok &= expect(write_file(config_path, "[core]\n"
	                                     "disabled = false\n"
	                                     "\n"
	                                     "[video]\n"
	                                     "dark_threshold = 60\n"),
	             "write initial config");
	const auto parentless_check = howdy::native::check_secure_config_path("config.ini");
	ok &= expect(!parentless_check.ok &&
	                 parentless_check.error_message.contains("must have a parent directory"),
	             "check_secure_config_path rejects parentless config path");

	const auto lines = howdy::native::read_config_lines(config_path, false);
	ok &= expect(lines.size() == 5, "read_config_lines returns expected line count");
	ok &= expect(!lines.empty() && lines[0] == "[core]\n", "read_config_lines preserves newlines");

	const std::string config_at_limit(howdy::native::kMaxConfigFileSize, 'x');
	const std::string config_over_limit(howdy::native::kMaxConfigFileSize + 1, 'x');
	ok &= expect(write_file(config_path, config_at_limit), "write config exactly at size limit");
	const auto limit_lines = howdy::native::read_config_lines(config_path, false);
	ok &= expect(limit_lines.size() == 1 && limit_lines.front() == config_at_limit,
	             "read_config_lines accepts config exactly at size limit");
	ok &= expect(write_file(config_path, config_over_limit), "write oversized config");
	ok &= expect(howdy::native::read_config_lines(config_path, false).empty(),
	             "read_config_lines rejects oversized config");
	std::string size_error;
	ok &= expect(!howdy::native::update_config_value(config_path, "disabled", &size_error, "true",
	                                                 false, false),
	             "update_config_value rejects oversized current config");
	ok &= expect(size_error == "Failed to read config file",
	             "oversized update reports config read failure");
	ok &= expect(read_file(config_path) == config_over_limit,
	             "oversized update leaves current config unchanged");
	ok &= expect(write_file(config_path, "[core]\n"
	                                     "disabled = false\n"
	                                     "\n"
	                                     "[video]\n"
	                                     "dark_threshold = 60\n"),
	             "restore initial config after size-limit reads");

	ok &= expect(howdy::native::update_config_value(config_path, "disabled", "true"),
	             "update_config_value succeeds for existing key");
	const auto after_disabled = read_file(config_path);
	ok &= expect(after_disabled.contains("disabled = true\n"), "disabled key updated");

	ok &= expect(howdy::native::is_safe_ini_scalar_value("true"),
	             "is_safe_ini_scalar_value accepts simple scalar");
	ok &= expect(howdy::native::is_safe_ini_scalar_value(""),
	             "is_safe_ini_scalar_value accepts empty scalar");
	ok &= expect(!howdy::native::is_safe_ini_scalar_value(std::string(1, '\0')),
	             "is_safe_ini_scalar_value rejects embedded NUL");
	ok &= expect(!howdy::native::is_safe_ini_scalar_value("\r"),
	             "is_safe_ini_scalar_value rejects carriage return");
	ok &= expect(!howdy::native::is_safe_ini_scalar_value("true\n[video]\ntimeout = 0"),
	             "is_safe_ini_scalar_value rejects newline injection");
	ok &= expect(!howdy::native::is_safe_ini_scalar_value("[video]"),
	             "is_safe_ini_scalar_value rejects section-like values");

	ok &= expect(howdy::native::update_config_value(config_path, "dark_threshold", "42"),
	             "update_config_value succeeds in later section");
	const auto after_threshold = read_file(config_path);
	ok &= expect(after_threshold.contains("dark_threshold = 42\n"), "dark_threshold key updated");

	const auto whitespace_path = temp_root / "whitespace.ini";
	ok &= expect(write_file(whitespace_path, "   \n\t  "), "write whitespace-only config lines");
	ok &= expect(!howdy::native::update_config_value(whitespace_path, "missing_key", nullptr, "x",
	                                                 false, false),
	             "update_config_value ignores whitespace-only lines");
	const auto alternate_syntax_path = temp_root / "alternate-syntax.ini";
	ok &= expect(write_file(alternate_syntax_path, "[core]\ndisabled true\n"),
	             "write space-separated config option");
	ok &= expect(howdy::native::update_config_value(alternate_syntax_path, "disabled", nullptr,
	                                                "false", false, false),
	             "update_config_value accepts space-separated option syntax");
	ok &= expect(read_file(alternate_syntax_path) == "[core]\ndisabled = false\n",
	             "space-separated option is normalized");

	ok &= expect(!howdy::native::update_config_value(config_path, "missing_key", "x"),
	             "update_config_value fails for missing key");
	ok &= expect(!howdy::native::update_config_value(config_path, "dark_threshold",
	                                                 "0\n[core]\ndisabled = true"),
	             "update_config_value rejects newline injection");
	ok &= expect(read_file(config_path) == after_threshold,
	             "rejected injection leaves config unchanged");
	ok &= expect(!howdy::native::update_config_value(config_path, "dark_threshold", "1000"),
	             "update_config_value rejects semantically invalid values");
	ok &= expect(read_file(config_path) == after_threshold,
	             "semantic validation failure leaves config unchanged");
	ok &= expect(!howdy::native::update_config_value(config_path, "device_fps", "-1"),
	             "update_config_value rejects invalid device_fps values");
	ok &= expect(read_file(config_path) == after_threshold,
	             "invalid device_fps update leaves config unchanged");

	ok &= expect_float_write_paths(config_path);

	ok &= expect(write_file(config_path, "[core]\n"
	                                     "disabled = false\n"
	                                     "\n"
	                                     "[video]\n"
	                                     "timeout = 0\n"),
	             "write config with invalid timeout");
	ok &= expect(
	    howdy::native::update_config_value(config_path, "disabled", nullptr, "true", false, false),
	    "update_config_value can bypass runtime validation for recovery writes");
	const auto after_disable_recovery = read_file(config_path);
	ok &= expect(after_disable_recovery.contains("disabled = true\n"),
	             "recovery write updates disabled key");
	howdy::native::ConfigReader still_invalid(config_path.string());
	ok &= expect(still_invalid.ok(), "recovery-write config still parses");
	ok &= expect(howdy::native::validate_runtime_config(still_invalid).has_value(),
	             "recovery write does not mask unrelated invalid config values");
	ok &= expect(write_file(config_path, after_threshold),
	             "restore valid config after recovery-write test");

	const auto                     nested_path = temp_root / "nested" / "generated.ini";
	const std::vector<std::string> write_lines = {
	    "[face]\n",
	    "sface_threshold = 0.363\n",
	};
	ok &= expect(howdy::native::atomic_file_commit_is_durable(
	                 howdy::native::atomic_write_lines(nested_path, write_lines)),
	             "atomic_write_lines creates parent dirs and writes file");
	ok &= expect(read_file(nested_path) == "[face]\nsface_threshold = 0.363\n",
	             "atomic_write_lines output matches expected content");
	const auto atomic_directory_path = temp_root / "atomic-directory.ini";
	ok &= expect(fs::create_directory(atomic_directory_path, ec),
	             "create non-regular atomic write target");
	ok &= expect(howdy::native::atomic_write_lines(atomic_directory_path, write_lines) ==
	                 howdy::native::AtomicFileCommitResult::kNotCommitted,
	             "atomic_write_lines rejects non-regular target");
	const auto         atomic_size_path = temp_root / "atomic-size-limit.ini";
	FileSizeLimitGuard atomic_file_size_limit;
	const bool         atomic_limit_set = atomic_file_size_limit.set_zero();
	if (atomic_limit_set) {
		ok &= expect(howdy::native::atomic_write_lines(atomic_size_path, write_lines) ==
		                 howdy::native::AtomicFileCommitResult::kNotCommitted,
		             "atomic_write_lines reports staged write failure");
		ok &= expect(atomic_file_size_limit.restore(),
		             "restore file-size limit after atomic write failure");
	}
	const auto uncertain_lines_path = temp_root / "uncertain-lines.ini";
	const auto uncertain_lines_result =
	    howdy::native::atomic_write_lines(uncertain_lines_path, write_lines, fail_parent_sync);
	ok &= expect(uncertain_lines_result ==
	                 howdy::native::AtomicFileCommitResult::kCommittedSyncFailed,
	             "atomic_write_lines reports committed parent-sync failure");
	ok &= expect(read_file(uncertain_lines_path) == "[face]\nsface_threshold = 0.363\n",
	             "atomic_write_lines leaves committed content visible after sync failure");

	const std::string valid_content = "[core]\n"
	                                  "disabled = false\n"
	                                  "\n"
	                                  "[video]\n"
	                                  "dark_threshold = 50\n";
	std::string       validation_error;
	ok &= expect(!howdy::native::validate_config_content(config_over_limit, &validation_error),
	             "validate_config_content rejects oversized content");
	ok &= expect(validation_error == "Updated config exceeds maximum size",
	             "oversized content reports size validation error");
	validation_error.clear();
	ok &= expect(!howdy::native::validate_config_content(config_over_limit, nullptr),
	             "validate_config_content rejects oversized content without error output");
	ok &= expect(howdy::native::validate_config_content(valid_content, &validation_error),
	             "validate_config_content accepts valid config");
	validation_error.clear();
	ok &= expect(!howdy::native::validate_config_content("[core\n", &validation_error),
	             "validate_config_content rejects invalid syntax");
	ok &= expect(validation_error == "Updated config is invalid",
	             "invalid config syntax reports stable invalid-config error");
	validation_error.clear();
	ok &=
	    expect(!howdy::native::validate_config_content("[video]\ntimeout = 0\n", &validation_error),
	           "validate_config_content rejects invalid runtime semantics");
	ok &= expect(!validation_error.empty(), "invalid runtime config reports an error message");
	ok &= expect(validation_error != "Updated config is invalid",
	             "invalid runtime config reports runtime validation error");

	const auto replace_path = temp_root / "replace.ini";
	ok &= expect(write_file(replace_path, valid_content), "write replace config baseline");
	ok &= expect(chmod(replace_path.c_str(), 0600) == 0, "set replace config mode");
	const std::string replacement_content = "[core]\n"
	                                        "disabled = true\n"
	                                        "\n"
	                                        "[video]\n"
	                                        "dark_threshold = 55\n";
	std::string       install_error;
	const auto        before_oversized_replace = read_file(replace_path);
	ok &= expect(!howdy::native::replace_config_content_atomically(replace_path, config_over_limit,
	                                                               &install_error, false, false),
	             "replace_config_content_atomically rejects oversized content");
	ok &= expect(install_error == "Updated config exceeds maximum size" &&
	                 read_file(replace_path) == before_oversized_replace,
	             "oversized replacement leaves existing config unchanged");
	ok &= expect(howdy::native::replace_config_content_atomically(replace_path, replacement_content,
	                                                              &install_error, true, true),
	             "replace_config_content_atomically installs valid content with lock");
	ok &= expect(read_file(replace_path) == replacement_content,
	             "replace_config_content_atomically writes expected content");
	struct stat replace_stat{};
	ok &= expect(stat(replace_path.c_str(), &replace_stat) == 0, "stat replaced config");
	ok &= expect((replace_stat.st_mode & 0777) == 0600,
	             "replace_config_content_atomically preserves config mode");

	const std::string uncertain_replacement = "[core]\n"
	                                          "disabled = false\n"
	                                          "\n"
	                                          "[video]\n"
	                                          "dark_threshold = 60\n";
	ok &= expect(!howdy::native::replace_config_content_atomically(
	                 replace_path, uncertain_replacement, &install_error, true, true, nullptr,
	                 fail_parent_sync),
	             "replace_config_content_atomically reports parent-sync failure");
	ok &= expect(install_error ==
	                 "Config was installed, but its directory could not be synced; verify state "
	                 "before retrying",
	             "replace config parent-sync failure reports committed state");
	ok &= expect(read_file(replace_path) == uncertain_replacement,
	             "replace config parent-sync failure leaves committed content visible");
	ok &= expect(count_staged_configs(temp_root) == 0,
	             "replace config parent-sync failure leaves no staged file");
	ok &= expect(howdy::native::replace_config_content_atomically(replace_path, replacement_content,
	                                                              &install_error, true, true),
	             "replace config recovers after parent-sync failure test");
	ok &= expect(
	    !howdy::native::replace_config_content_atomically(
	        replace_path, uncertain_replacement, &install_error, false, false, nullptr, nullptr),
	    "replace config reports null parent-sync callback as nondurable");
	ok &= expect(install_error.contains("could not be synced") &&
	                 read_file(replace_path) == uncertain_replacement,
	             "null parent-sync callback leaves committed content visible");
	ok &= expect(howdy::native::replace_config_content_atomically(replace_path, replacement_content,
	                                                              &install_error, false, false),
	             "replace config recovers after null parent-sync callback");

	const auto before_invalid_replace = read_file(replace_path);
	const auto staged_before          = count_staged_configs(temp_root);
	ok &= expect(!howdy::native::replace_config_content_atomically(
	                 replace_path, "[video]\ntimeout = 0\n", &install_error, true, true),
	             "replace_config_content_atomically rejects invalid content with lock");
	ok &= expect(read_file(replace_path) == before_invalid_replace,
	             "invalid replacement leaves old config unchanged");
	ok &= expect(count_staged_configs(temp_root) == staged_before,
	             "invalid replacement leaves no staged config");

	const std::string recovery_content = "[core]\n"
	                                     "disabled = true\n"
	                                     "\n"
	                                     "[video]\n"
	                                     "timeout = 0\n";
	ok &= expect(howdy::native::replace_config_content_atomically(replace_path, recovery_content,
	                                                              &install_error, true, false),
	             "replace_config_content_atomically releases lock and can bypass validation");
	ok &= expect(read_file(replace_path) == recovery_content,
	             "runtime-validation bypass installs content");

	const auto &stale_expected_content = recovery_content;
	ok &= expect(write_file(replace_path, valid_content), "write changed config before stale edit");
	ok &= expect(chmod(replace_path.c_str(), 0600) == 0, "restore changed config mode");
	ok &= expect(!howdy::native::replace_config_content_atomically(
	                 replace_path, replacement_content, &install_error, true, false,
	                 &stale_expected_content),
	             "replace_config_content_atomically rejects stale expected content");
	ok &= expect(read_file(replace_path) == valid_content,
	             "stale expected content leaves current config unchanged");

	ok &=
	    expect(write_file(replace_path, config_over_limit), "write oversized stale current config");
	ok &= expect(
	    !howdy::native::replace_config_content_atomically(
	        replace_path, replacement_content, &install_error, true, false, &config_over_limit),
	    "stale comparison rejects oversized current config");
	ok &= expect(install_error == "Failed to read config file",
	             "oversized stale comparison reports config read failure");
	ok &= expect(read_file(replace_path) == config_over_limit,
	             "oversized stale comparison performs no install");
	ok &= expect(write_file(replace_path, valid_content),
	             "restore replace config after oversized stale comparison");

	ok &= expect(chmod(replace_path.c_str(), 0666) == 0, "make replace config world-writable");
	ok &= expect(!howdy::native::replace_config_content_atomically(replace_path, valid_content,
	                                                               &install_error, true, true),
	             "replace_config_content_atomically rejects insecure config permissions with lock");
	ok &= expect(chmod(replace_path.c_str(), 0600) == 0, "restore replace config permissions");

	const auto lock_failure_path = lock_path_for_config(config_path);
	fs::remove(lock_failure_path, ec);
	ec.clear();
	fs::create_symlink("/tmp", lock_failure_path, ec);
	ok &= expect(!ec, "create config lock symlink for lock failure");
	ok &= expect(howdy::native::read_config_lines(config_path, true).empty(),
	             "read_config_lines fails closed when lock cannot be opened");
	std::string lock_error;
	ok &= expect(!howdy::native::update_config_value(config_path, "disabled", &lock_error, "false",
	                                                 true, false) &&
	                 lock_error == "Failed to lock config file",
	             "update_config_value reports config lock failure");
	fs::remove(lock_failure_path, ec);
	ec.clear();

	const auto replace_insecure_dir = temp_root / "replace-insecure-dir";
	ok &= expect(fs::create_directories(replace_insecure_dir, ec) || !ec,
	             "create insecure replace dir");
	ok &= expect(!ec, "no error creating insecure replace dir");
	const auto replace_insecure_path = replace_insecure_dir / "config.ini";
	ok &= expect(write_file(replace_insecure_path, valid_content), "write insecure replace config");
	ok &= expect(chmod(replace_insecure_dir.c_str(), 0777) == 0,
	             "make replace config dir world-writable");
	ok &= expect(!howdy::native::replace_config_content_atomically(
	                 replace_insecure_path, replacement_content, &install_error, false, true),
	             "replace_config_content_atomically rejects insecure parent directory");
	ok &= expect(chmod(replace_insecure_dir.c_str(), 0755) == 0, "restore replace config dir mode");

	const auto non_regular_path = temp_root / "non-regular.ini";
	ok &= expect(fs::create_directory(non_regular_path, ec), "create non-regular config target");
	ok &= expect(!howdy::native::replace_config_content_atomically(non_regular_path, valid_content,
	                                                               &install_error, false, true),
	             "replace_config_content_atomically rejects non-regular config target");

	const auto install_failure_dir = temp_root / "install-failure";
	ok &= expect(fs::create_directories(install_failure_dir, ec) || !ec,
	             "create install-failure directory");
	ok &= expect(!ec, "no error creating install-failure directory");
	const auto install_failure_path = install_failure_dir / "config.ini";
	ok &= expect(write_file(install_failure_path, "[core]\ndisabled = false\n"),
	             "write install-failure config");
	std::string update_error;
	bool        update_failed = false;
	const auto  install_failure_security =
	    howdy::native::check_secure_config_path(install_failure_path);
	ok &= expect(install_failure_security.ok,
	             "install-failure config passes secure path check before update");
	FileSizeLimitGuard file_size_limit_guard;
	const bool         have_file_size_limit = file_size_limit_guard.have_original;
	ok &= expect(have_file_size_limit, "read file-size limit for install-failure test");
	const bool set_file_size_limit = file_size_limit_guard.set_zero();
	ok &= expect(set_file_size_limit, "set file-size limit for install-failure test");
	if (install_failure_security.ok && set_file_size_limit) {
		update_failed = !howdy::native::update_config_value(install_failure_path, "disabled",
		                                                    &update_error, "true", false, false);
	}
	if (set_file_size_limit) {
		ok &= expect(file_size_limit_guard.restore(),
		             "restore file-size limit after install-failure test");
	}
	ok &= expect(update_failed, "update_config_value returns false when install fails");
	ok &= expect(update_error == "Failed to update config file",
	             "failed update_config_value install reports fallback error: " + update_error);

	const auto config_lock_path = lock_path_for_config(config_path);
	fs::remove(config_lock_path, ec);
	ok &= expect(!ec && !fs::exists(config_lock_path),
	             "config lock absent before insecure config update");
	ok &= expect(chmod(config_path.c_str(), 0666) == 0, "make config file world-writable");
	ok &=
	    expect(!howdy::native::update_config_value(config_path, "disabled", nullptr, "false", true),
	           "update_config_value rejects insecure config permissions");
	ok &= expect(!fs::exists(config_lock_path),
	             "update_config_value rejects insecure config before creating lock");
	ok &= expect(chmod(config_path.c_str(), 0644) == 0, "restore config permissions");

	const auto insecure_dir = temp_root / "insecure-dir";
	ok &= expect(fs::create_directories(insecure_dir, ec) || !ec, "create insecure config dir");
	ok &= expect(!ec, "no error creating insecure config dir");
	const auto insecure_config_path = insecure_dir / "config.ini";
	ok &= expect(write_file(insecure_config_path, "[core]\ndisabled = false\n"),
	             "write config in insecure dir");
	const auto insecure_config_lock_path = lock_path_for_config(insecure_config_path);
	fs::remove(insecure_config_lock_path, ec);
	ok &= expect(!ec && !fs::exists(insecure_config_lock_path),
	             "config lock absent before insecure directory update");
	ok &= expect(chmod(insecure_dir.c_str(), 0777) == 0, "make config dir world-writable");
	ok &= expect(!howdy::native::check_secure_config_path(insecure_config_path).ok,
	             "check_secure_config_path rejects insecure config directory");
	ok &= expect(!howdy::native::update_config_value(insecure_config_path, "disabled", nullptr,
	                                                 "true", true),
	             "update_config_value rejects insecure config directory");
	ok &= expect(!fs::exists(insecure_config_lock_path),
	             "update_config_value rejects insecure directory before creating lock");
	ok &= expect(chmod(insecure_dir.c_str(), 0755) == 0, "restore config dir mode");

	const auto insecure_ancestor_root = temp_root / "insecure-ancestor";
	const auto nested_config_dir      = insecure_ancestor_root / "nested" / "deeper";
	ok &= expect(fs::create_directories(nested_config_dir, ec) || !ec, "create nested config dir");
	ok &= expect(!ec, "no error creating nested config dir");
	const auto nested_config_path = nested_config_dir / "config.ini";
	ok &= expect(write_file(nested_config_path, "[core]\ndisabled = false\n"),
	             "write config in nested dir");
	const auto nested_config_lock_path = lock_path_for_config(nested_config_path);
	fs::remove(nested_config_lock_path, ec);
	ok &= expect(!ec && !fs::exists(nested_config_lock_path),
	             "config lock absent before insecure ancestor update");
	ok &= expect(chmod(insecure_ancestor_root.c_str(), 0777) == 0,
	             "make ancestor config dir world-writable");
	ok &= expect(!howdy::native::check_secure_config_path(nested_config_path).ok,
	             "check_secure_config_path rejects insecure ancestor directory");
	ok &= expect(
	    !howdy::native::update_config_value(nested_config_path, "disabled", nullptr, "true", true),
	    "update_config_value rejects insecure ancestor directory");
	ok &= expect(!fs::exists(nested_config_lock_path),
	             "update_config_value rejects insecure ancestor before creating lock");
	ok &= expect(chmod(insecure_ancestor_root.c_str(), 0755) == 0,
	             "restore ancestor config dir mode");

	const auto unreadable_dir = temp_root / "unreadable-dir";
	ok &= expect(fs::create_directories(unreadable_dir, ec) || !ec, "create unreadable config dir");
	ok &= expect(!ec, "no error creating unreadable config dir");
	const auto unreadable_config_path = unreadable_dir / "config.ini";
	ok &= expect(write_file(unreadable_config_path, "[core]\ndisabled = false\n"),
	             "write config in unreadable dir");
	ok &= expect(chmod(unreadable_dir.c_str(), 0000) == 0, "make config dir unreadable");
	if (geteuid() != 0) {
		const auto unreadable_check =
		    howdy::native::check_secure_config_path(unreadable_config_path);
		ok &= expect(!unreadable_check.ok,
		             "check_secure_config_path rejects inaccessible config path");
		ok &= expect(unreadable_check.error_code == EACCES,
		             "inaccessible config path preserves EACCES");
		ok &= expect(unreadable_check.error_message.contains("process uid="),
		             "inaccessible config path reports process uid");
		ok &= expect(unreadable_check.error_message.contains("do not make /etc/howdy"),
		             "inaccessible config path warns against insecure permissions");
	}
	ok &= expect(chmod(unreadable_dir.c_str(), 0755) == 0, "restore unreadable config dir mode");

	const auto protected_path = temp_root / "protected.ini";
	ok &=
	    expect(write_file(protected_path, "[core]\ndisabled = false\n"), "write protected config");
	ok &= expect(chmod(protected_path.c_str(), 0600) == 0, "set protected config mode");
	const auto strict_root_check =
	    howdy::native::check_secure_config_path(protected_path, static_cast<uid_t>(0));
	if (geteuid() != 0) {
		ok &= expect(!strict_root_check.ok,
		             "strict root-owned config check rejects non-root-owned config");
		ok &= expect(strict_root_check.error_message.contains("owned by UID 0"),
		             "strict root-owned config check reports root ownership requirement");
	} else {
		ok &= expect(strict_root_check.ok,
		             "strict root-owned config check accepts root-owned config");
	}
	ok &= expect(howdy::native::update_config_value(protected_path, "disabled", "true"),
	             "update_config_value succeeds on secure config");
	struct stat protected_stat{};
	ok &= expect(stat(protected_path.c_str(), &protected_stat) == 0, "stat protected config");
	ok &= expect((protected_stat.st_mode & 0777) == 0600, "atomic write preserves config mode");

	const auto hardlink_path = temp_root / "protected-hardlink.ini";
	ok &= expect(link(protected_path.c_str(), hardlink_path.c_str()) == 0,
	             "create hard link to protected config");
	ok &= expect(!howdy::native::update_config_value(hardlink_path, "disabled", "false"),
	             "update_config_value rejects hard-linked config");

	howdy::native::ConfigReader validated(config_path.string());
	ok &= expect(validated.ok(), "validated config still parses");
	ok &= expect(!howdy::native::validate_runtime_config(validated).has_value(),
	             "validated config passes semantic validation");

	fs::remove_all(temp_root, ec);
	if (!ok) {
		return 1;
	}
	return 0;
}
