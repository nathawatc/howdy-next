#include "config/config_utils.hpp"
#include "config/config_utils_test_support.hpp"
#include "support/bit_flags.hpp"
#include "test_support.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

#include <sys/stat.h>

namespace howdy::test {
	namespace {
		auto CountStagedConfigs(const std::filesystem::path &directory) -> std::size_t {
			return CountFilesWithPrefix(directory, ".howdy-config-");
		}

	}  // namespace

	auto RunConfigAtomicReplaceTests(ConfigUtilsTestContext &context) -> bool {
		using howdy::test::Expect;
		bool              ok            = true;
		const std::string valid_content = "[core]\n"
		                                  "disabled = false\n"
		                                  "\n"
		                                  "[video]\n"
		                                  "dark_threshold = 50\n";
		using howdy::native::ConfigValidationStatus;
		const auto oversized = howdy::native::ValidateConfigContent(context.config_over_limit);
		ok &= Expect(oversized.status == ConfigValidationStatus::kTooLarge,
		             "validate_config_content rejects oversized content");
		ok &= Expect(oversized.message == "Updated config exceeds maximum size",
		             "oversized content reports size validation error");
		ok &= Expect(howdy::native::ValidateConfigContent(valid_content).status ==
		                 howdy::native::ConfigValidationStatus::kOk,
		             "validate_config_content accepts valid config");
		{
			const ScopedEnvironmentVariable tmpdir_env(
			    "TMPDIR", (context.temp_root / "missing-tmpdir").string());
			ok &= Expect(howdy::native::ValidateConfigContent(valid_content).status ==
			                 howdy::native::ConfigValidationStatus::kOk,
			             "validate_config_content parses in memory without TMPDIR");
		}
		for (const auto &[damaged_content, message] : {
		         std::pair{std::string(), "Config is blank"},
		         std::pair{valid_content + '\0', "Config contains a NUL byte"},
		     }) {
			const auto damaged = howdy::native::ValidateConfigContent(damaged_content);
			ok &= Expect(damaged.status == ConfigValidationStatus::kDamaged &&
			                 damaged.message == message,
			             "validate_config_content rejects damaged content: " + damaged.message);

			const auto damage_path = context.temp_root / "replace-damage.ini";
			ok &= Expect(WriteConfigTestFile(damage_path, valid_content), "write damage baseline");
			const auto replaced = howdy::native::ReplaceConfigContentAtomically(
			    damage_path, damaged_content,
			    {.lock = false, .validate_runtime = false, .validation_root = {context.temp_root}});
			ok &= Expect(replaced.status == howdy::native::ConfigWriteStatus::kInvalidConfig &&
			                 replaced.message == message && ReadFile(damage_path) == valid_content,
			             "damaged content is never installed: " + replaced.message);
		}
		const auto unparsable = howdy::native::ValidateConfigContent("[core\n");
		ok &= Expect(unparsable.status == ConfigValidationStatus::kUnparsable,
		             "validate_config_content rejects invalid syntax");
		ok &= Expect(unparsable.message == "Updated config is invalid",
		             "invalid config syntax reports stable invalid-config error");
		const auto invalid_setting = howdy::native::ValidateConfigContent("[video]\ntimeout = 0\n");
		ok &= Expect(invalid_setting.status == ConfigValidationStatus::kInvalidSetting,
		             "validate_config_content rejects invalid runtime semantics");
		ok &= Expect(invalid_setting.message.contains("timeout"),
		             "invalid runtime config names the invalid setting");
		ok &= Expect(
		    howdy::native::ConfigValidationResult{}.status != ConfigValidationStatus::kOk &&
		        howdy::native::ConfigWriteResult{}.status != howdy::native::ConfigWriteStatus::kOk,
		    "a result that names no status is a failure");

		const auto replace_path = context.temp_root / "replace.ini";
		ok &= Expect(WriteConfigTestFile(replace_path, valid_content),
		             "write replace config baseline");
		ok &= Expect(chmod(replace_path.c_str(), 0600) == 0, "set replace config mode");
		const std::string replacement_content      = "[core]\n"
		                                             "disabled = true\n"
		                                             "\n"
		                                             "[video]\n"
		                                             "dark_threshold = 55\n";
		const auto        before_oversized_replace = ReadFile(replace_path);
		auto              install_result           = howdy::native::ReplaceConfigContentAtomically(
		    replace_path, context.config_over_limit,
		    {.lock = false, .validate_runtime = false, .validation_root = {context.temp_root}});
		ok &= Expect(install_result.status == howdy::native::ConfigWriteStatus::kInvalidConfig,
		             "replace_config_content_atomically rejects oversized content");
		ok &= Expect(install_result.message == "Updated config exceeds maximum size" &&
		                 ReadFile(replace_path) == before_oversized_replace,
		             "oversized replacement leaves existing config unchanged");
		const auto missing_path = context.temp_root / "missing-replace-config.ini";
		install_result          = howdy::native::ReplaceConfigContentAtomically(
		    missing_path, replacement_content, {.validation_root = {context.temp_root}});
		ok &= Expect(install_result.status == howdy::native::ConfigWriteStatus::kReadFailed &&
		                 !std::filesystem::exists(missing_path),
		             "replace_config_content_atomically does not create a missing config");
		install_result = howdy::native::ReplaceConfigContentAtomically(
		    replace_path, replacement_content, {.validation_root = {context.temp_root}});
		ok &= Expect(install_result.status == howdy::native::ConfigWriteStatus::kOk,
		             "replace_config_content_atomically installs valid content with lock");
		ok &= Expect(ReadFile(replace_path) == replacement_content,
		             "replace_config_content_atomically writes expected content");
		struct stat replace_stat{};
		ok &= Expect(stat(replace_path.c_str(), &replace_stat) == 0, "stat replaced config");
		ok &= Expect(howdy::native::MaskedBits(replace_stat.st_mode, 0777) == 0600,
		             "replace_config_content_atomically preserves config mode");

		const std::string uncertain_replacement = "[core]\n"
		                                          "disabled = false\n"
		                                          "\n"
		                                          "[video]\n"
		                                          "dark_threshold = 60\n";
		install_result                          = howdy::native::ReplaceConfigContentAtomically(
		    replace_path, uncertain_replacement,
		    {.sync_parent = FailParentSync, .validation_root = {context.temp_root}});
		ok &=
		    Expect(install_result.status == howdy::native::ConfigWriteStatus::kDurabilityUncertain,
			       "replace_config_content_atomically reports parent-sync failure");
		ok &=
		    Expect(install_result.message ==
			           "Config was installed, but its directory could not be synced; verify state "
			           "before retrying",
			       "replace config parent-sync failure reports committed state");
		ok &= Expect(ReadFile(replace_path) == uncertain_replacement,
		             "replace config parent-sync failure leaves committed content visible");
		ok &= Expect(CountStagedConfigs(context.temp_root) == 0,
		             "replace config parent-sync failure leaves no staged file");
		install_result = howdy::native::ReplaceConfigContentAtomically(
		    replace_path, replacement_content, {.validation_root = {context.temp_root}});
		ok &= Expect(install_result.status == howdy::native::ConfigWriteStatus::kOk,
		             "replace config recovers after parent-sync failure test");
		install_result = howdy::native::ReplaceConfigContentAtomically(
		    replace_path, uncertain_replacement,
		    {
		        .lock             = false,
		        .validate_runtime = false,
		        .sync_parent      = nullptr,
		        .validation_root  = {context.temp_root},
		    });
		ok &=
		    Expect(install_result.status == howdy::native::ConfigWriteStatus::kDurabilityUncertain,
			       "replace config reports null parent-sync callback as nondurable");
		ok &= Expect(install_result.message.contains("could not be synced") &&
		                 ReadFile(replace_path) == uncertain_replacement,
		             "null parent-sync callback leaves committed content visible");
		install_result = howdy::native::ReplaceConfigContentAtomically(
		    replace_path, replacement_content,
		    {.lock = false, .validate_runtime = false, .validation_root = {context.temp_root}});
		ok &= Expect(install_result.status == howdy::native::ConfigWriteStatus::kOk,
		             "replace config recovers after null parent-sync callback");

		const auto before_invalid_replace = ReadFile(replace_path);
		const auto staged_before          = CountStagedConfigs(context.temp_root);
		install_result                    = howdy::native::ReplaceConfigContentAtomically(
		    replace_path, "[video]\ntimeout = 0\n", {.validation_root = {context.temp_root}});
		ok &= Expect(install_result.status == howdy::native::ConfigWriteStatus::kInvalidConfig,
		             "replace_config_content_atomically rejects invalid content with lock");
		ok &= Expect(ReadFile(replace_path) == before_invalid_replace,
		             "invalid replacement leaves old config unchanged");
		ok &= Expect(CountStagedConfigs(context.temp_root) == staged_before,
		             "invalid replacement leaves no staged config");

		const std::string recovery_content = "[core]\n"
		                                     "disabled = true\n"
		                                     "\n"
		                                     "[video]\n"
		                                     "timeout = 0\n";
		install_result                     = howdy::native::ReplaceConfigContentAtomically(
		    replace_path, recovery_content,
		    {.validate_runtime = false, .validation_root = {context.temp_root}});
		ok &= Expect(install_result.status == howdy::native::ConfigWriteStatus::kOk,
		             "replace_config_content_atomically releases lock and can bypass validation");
		ok &= Expect(ReadFile(replace_path) == recovery_content,
		             "runtime-validation bypass installs content");

		const auto &stale_expected_content = recovery_content;
		ok &= Expect(WriteConfigTestFile(replace_path, valid_content),
		             "write changed config before stale edit");
		ok &= Expect(chmod(replace_path.c_str(), 0600) == 0, "restore changed config mode");
		install_result = howdy::native::ReplaceConfigContentAtomically(
		    replace_path, replacement_content,
		    {
		        .validate_runtime         = false,
		        .expected_current_content = &stale_expected_content,
		        .validation_root          = {context.temp_root},
		    });
		ok &= Expect(install_result.status == howdy::native::ConfigWriteStatus::kStale,
		             "replace_config_content_atomically rejects stale expected content");
		ok &= Expect(ReadFile(replace_path) == valid_content,
		             "stale expected content leaves current config unchanged");

		ok &= Expect(WriteConfigTestFile(replace_path, context.config_over_limit),
		             "write oversized stale current config");
		install_result = howdy::native::ReplaceConfigContentAtomically(
		    replace_path, replacement_content,
		    {
		        .validate_runtime         = false,
		        .expected_current_content = &context.config_over_limit,
		        .validation_root          = {context.temp_root},
		    });
		ok &= Expect(install_result.status == howdy::native::ConfigWriteStatus::kReadFailed,
		             "stale comparison rejects oversized current config");
		ok &= Expect(install_result.message == "Failed to read config file",
		             "oversized stale comparison reports config read failure");
		ok &= Expect(ReadFile(replace_path) == context.config_over_limit,
		             "oversized stale comparison performs no install");
		ok &= Expect(WriteConfigTestFile(replace_path, valid_content),
		             "restore replace config after oversized stale comparison");

		ok &= Expect(chmod(replace_path.c_str(), 0666) == 0, "make replace config world-writable");
		install_result = howdy::native::ReplaceConfigContentAtomically(
		    replace_path, valid_content, {.validation_root = {context.temp_root}});
		ok &= Expect(
		    install_result.status == howdy::native::ConfigWriteStatus::kInsecurePath,
		    "replace_config_content_atomically rejects insecure config permissions with lock");
		ok &= Expect(chmod(replace_path.c_str(), 0600) == 0, "restore replace config permissions");

		return ok;
	}

	auto RunConfigAtomicReplaceTailTests(ConfigUtilsTestContext &context) -> bool {
		namespace fs = std::filesystem;
		using howdy::test::Expect;
		bool              ok = true;
		std::error_code   ec;
		const std::string valid_content        = "[core]\n"
		                                         "disabled = false\n"
		                                         "\n"
		                                         "[video]\n"
		                                         "dark_threshold = 50\n";
		const std::string replacement_content  = "[core]\n"
		                                         "disabled = true\n"
		                                         "\n"
		                                         "[video]\n"
		                                         "dark_threshold = 55\n";
		const auto        replace_insecure_dir = context.temp_root / "replace-insecure-dir";
		ok &= Expect(fs::create_directories(replace_insecure_dir, ec) || !ec,
		             "create insecure replace dir");
		ok &= Expect(!ec, "no error creating insecure replace dir");
		const auto replace_insecure_path = replace_insecure_dir / "config.ini";
		ok &= Expect(WriteConfigTestFile(replace_insecure_path, valid_content),
		             "write insecure replace config");
		ok &= Expect(chmod(replace_insecure_dir.c_str(), 0777) == 0,
		             "make replace config dir world-writable");
		auto install_result = howdy::native::ReplaceConfigContentAtomically(
		    replace_insecure_path, replacement_content,
		    {.lock = false, .validation_root = {context.temp_root}});
		ok &= Expect(install_result.status == howdy::native::ConfigWriteStatus::kInsecurePath,
		             "replace_config_content_atomically rejects insecure parent directory");
		ok &= Expect(chmod(replace_insecure_dir.c_str(), 0755) == 0,
		             "restore replace config dir mode");

		if (geteuid() != 0) {  // root ignores the mode
			ok &= Expect(chmod(replace_insecure_dir.c_str(), 0555) == 0,
			             "make replace config dir read-only");
			install_result = howdy::native::ReplaceConfigContentAtomically(
			    replace_insecure_path, replacement_content,
			    {.lock = false, .validation_root = {context.temp_root}});
			ok &=
			    Expect(install_result.status == howdy::native::ConfigWriteStatus::kStagingFailed &&
				           install_result.message == "Failed to stage updated config",
				       "replace config reports a staging failure");
			const auto update_result = howdy::native::UpdateConfigValue(
			    replace_insecure_path, {.key = "disabled", .value = "true"},
			    {.lock = false, .validation_root = {context.temp_root}});
			ok &= Expect(update_result.status == howdy::native::ConfigWriteStatus::kStagingFailed &&
			                 update_result.message == "Failed to stage updated config",
			             "update_config_value keeps the staging failure message");
			ok &= Expect(ReadFile(replace_insecure_path) == valid_content,
			             "staging failure leaves the config unchanged");
			ok &= Expect(chmod(replace_insecure_dir.c_str(), 0755) == 0,
			             "restore writable replace config dir");
		}

		const auto non_regular_path = context.temp_root / "non-regular.ini";
		ok &=
		    Expect(fs::create_directory(non_regular_path, ec), "create non-regular config target");
		install_result = howdy::native::ReplaceConfigContentAtomically(
		    non_regular_path, valid_content,
		    {.lock = false, .validation_root = {context.temp_root}});
		ok &= Expect(install_result.status == howdy::native::ConfigWriteStatus::kInsecurePath,
		             "replace_config_content_atomically rejects non-regular config target");
		return ok;
	}

}  // namespace howdy::test
