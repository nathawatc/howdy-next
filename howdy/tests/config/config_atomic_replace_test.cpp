#include "config/config_utils.hpp"
#include "config/config_utils_test_support.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <string>

#include <sys/stat.h>

namespace howdy::test {
	namespace {
		auto count_staged_configs(const std::filesystem::path &directory) -> std::size_t {
			std::size_t count = 0;
			for (const auto &entry : std::filesystem::directory_iterator(directory)) {
				if (entry.path().filename().string().starts_with(".howdy-config-")) {
					++count;
				}
			}
			return count;
		}

	}  // namespace

	auto run_config_atomic_replace_tests(ConfigUtilsTestContext &context) -> bool {
		namespace fs = std::filesystem;
		using howdy::test::expect;
		bool              ok = true;
		std::error_code   ec;
		const std::string valid_content = "[core]\n"
		                                  "disabled = false\n"
		                                  "\n"
		                                  "[video]\n"
		                                  "dark_threshold = 50\n";
		std::string       validation_error;
		ok &= expect(
		    !howdy::native::validate_config_content(context.config_over_limit, &validation_error),
		    "validate_config_content rejects oversized content");
		ok &= expect(validation_error == "Updated config exceeds maximum size",
		             "oversized content reports size validation error");
		validation_error.clear();
		ok &= expect(!howdy::native::validate_config_content(context.config_over_limit, nullptr),
		             "validate_config_content rejects oversized content without error output");
		ok &= expect(howdy::native::validate_config_content(valid_content, &validation_error),
		             "validate_config_content accepts valid config");
		validation_error.clear();
		ok &= expect(!howdy::native::validate_config_content("[core\n", &validation_error),
		             "validate_config_content rejects invalid syntax");
		ok &= expect(validation_error == "Updated config is invalid",
		             "invalid config syntax reports stable invalid-config error");
		validation_error.clear();
		ok &= expect(
		    !howdy::native::validate_config_content("[video]\ntimeout = 0\n", &validation_error),
		    "validate_config_content rejects invalid runtime semantics");
		ok &= expect(!validation_error.empty(), "invalid runtime config reports an error message");
		ok &= expect(validation_error != "Updated config is invalid",
		             "invalid runtime config reports runtime validation error");

		const auto replace_path = context.temp_root / "replace.ini";
		ok &= expect(write_config_test_file(replace_path, valid_content),
		             "write replace config baseline");
		ok &= expect(chmod(replace_path.c_str(), 0600) == 0, "set replace config mode");
		const std::string replacement_content = "[core]\n"
		                                        "disabled = true\n"
		                                        "\n"
		                                        "[video]\n"
		                                        "dark_threshold = 55\n";
		std::string       install_error;
		const auto        before_oversized_replace = read_config_test_file(replace_path);
		ok &= expect(!howdy::native::replace_config_content_atomically(
		                 replace_path, context.config_over_limit, &install_error, false, false),
		             "replace_config_content_atomically rejects oversized content");
		ok &= expect(install_error == "Updated config exceeds maximum size" &&
		                 read_config_test_file(replace_path) == before_oversized_replace,
		             "oversized replacement leaves existing config unchanged");
		ok &= expect(howdy::native::replace_config_content_atomically(
		                 replace_path, replacement_content, &install_error, true, true),
		             "replace_config_content_atomically installs valid content with lock");
		ok &= expect(read_config_test_file(replace_path) == replacement_content,
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
		ok &=
		    expect(install_error ==
		               "Config was installed, but its directory could not be synced; verify state "
		               "before retrying",
		           "replace config parent-sync failure reports committed state");
		ok &= expect(read_config_test_file(replace_path) == uncertain_replacement,
		             "replace config parent-sync failure leaves committed content visible");
		ok &= expect(count_staged_configs(context.temp_root) == 0,
		             "replace config parent-sync failure leaves no staged file");
		ok &= expect(howdy::native::replace_config_content_atomically(
		                 replace_path, replacement_content, &install_error, true, true),
		             "replace config recovers after parent-sync failure test");
		ok &= expect(!howdy::native::replace_config_content_atomically(
		                 replace_path, uncertain_replacement, &install_error, false, false, nullptr,
		                 nullptr),
		             "replace config reports null parent-sync callback as nondurable");
		ok &= expect(install_error.contains("could not be synced") &&
		                 read_config_test_file(replace_path) == uncertain_replacement,
		             "null parent-sync callback leaves committed content visible");
		ok &= expect(howdy::native::replace_config_content_atomically(
		                 replace_path, replacement_content, &install_error, false, false),
		             "replace config recovers after null parent-sync callback");

		const auto before_invalid_replace = read_config_test_file(replace_path);
		const auto staged_before          = count_staged_configs(context.temp_root);
		ok &= expect(!howdy::native::replace_config_content_atomically(
		                 replace_path, "[video]\ntimeout = 0\n", &install_error, true, true),
		             "replace_config_content_atomically rejects invalid content with lock");
		ok &= expect(read_config_test_file(replace_path) == before_invalid_replace,
		             "invalid replacement leaves old config unchanged");
		ok &= expect(count_staged_configs(context.temp_root) == staged_before,
		             "invalid replacement leaves no staged config");

		const std::string recovery_content = "[core]\n"
		                                     "disabled = true\n"
		                                     "\n"
		                                     "[video]\n"
		                                     "timeout = 0\n";
		ok &= expect(howdy::native::replace_config_content_atomically(
		                 replace_path, recovery_content, &install_error, true, false),
		             "replace_config_content_atomically releases lock and can bypass validation");
		ok &= expect(read_config_test_file(replace_path) == recovery_content,
		             "runtime-validation bypass installs content");

		const auto &stale_expected_content = recovery_content;
		ok &= expect(write_config_test_file(replace_path, valid_content),
		             "write changed config before stale edit");
		ok &= expect(chmod(replace_path.c_str(), 0600) == 0, "restore changed config mode");
		ok &= expect(!howdy::native::replace_config_content_atomically(
		                 replace_path, replacement_content, &install_error, true, false,
		                 &stale_expected_content),
		             "replace_config_content_atomically rejects stale expected content");
		ok &= expect(read_config_test_file(replace_path) == valid_content,
		             "stale expected content leaves current config unchanged");

		ok &= expect(write_config_test_file(replace_path, context.config_over_limit),
		             "write oversized stale current config");
		ok &= expect(!howdy::native::replace_config_content_atomically(
		                 replace_path, replacement_content, &install_error, true, false,
		                 &context.config_over_limit),
		             "stale comparison rejects oversized current config");
		ok &= expect(install_error == "Failed to read config file",
		             "oversized stale comparison reports config read failure");
		ok &= expect(read_config_test_file(replace_path) == context.config_over_limit,
		             "oversized stale comparison performs no install");
		ok &= expect(write_config_test_file(replace_path, valid_content),
		             "restore replace config after oversized stale comparison");

		ok &= expect(chmod(replace_path.c_str(), 0666) == 0, "make replace config world-writable");
		ok &= expect(
		    !howdy::native::replace_config_content_atomically(replace_path, valid_content,
		                                                      &install_error, true, true),
		    "replace_config_content_atomically rejects insecure config permissions with lock");
		ok &= expect(chmod(replace_path.c_str(), 0600) == 0, "restore replace config permissions");

		return ok;
	}

	auto run_config_atomic_replace_tail_tests(ConfigUtilsTestContext &context) -> bool {
		namespace fs = std::filesystem;
		using howdy::test::expect;
		bool              ok = true;
		std::error_code   ec;
		const std::string valid_content       = "[core]\n"
		                                        "disabled = false\n"
		                                        "\n"
		                                        "[video]\n"
		                                        "dark_threshold = 50\n";
		const std::string replacement_content = "[core]\n"
		                                        "disabled = true\n"
		                                        "\n"
		                                        "[video]\n"
		                                        "dark_threshold = 55\n";
		std::string       install_error;
		const auto        replace_insecure_dir = context.temp_root / "replace-insecure-dir";
		ok &= expect(fs::create_directories(replace_insecure_dir, ec) || !ec,
		             "create insecure replace dir");
		ok &= expect(!ec, "no error creating insecure replace dir");
		const auto replace_insecure_path = replace_insecure_dir / "config.ini";
		ok &= expect(write_config_test_file(replace_insecure_path, valid_content),
		             "write insecure replace config");
		ok &= expect(chmod(replace_insecure_dir.c_str(), 0777) == 0,
		             "make replace config dir world-writable");
		ok &= expect(!howdy::native::replace_config_content_atomically(
		                 replace_insecure_path, replacement_content, &install_error, false, true),
		             "replace_config_content_atomically rejects insecure parent directory");
		ok &= expect(chmod(replace_insecure_dir.c_str(), 0755) == 0,
		             "restore replace config dir mode");

		const auto non_regular_path = context.temp_root / "non-regular.ini";
		ok &=
		    expect(fs::create_directory(non_regular_path, ec), "create non-regular config target");
		ok &= expect(!howdy::native::replace_config_content_atomically(
		                 non_regular_path, valid_content, &install_error, false, true),
		             "replace_config_content_atomically rejects non-regular config target");
		return ok;
	}

}  // namespace howdy::test
