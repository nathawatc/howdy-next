#include "../../src/config/config_reader/internal.hpp"
#include "../../src/config/config_validation/internal.hpp"
#include "config/config_utils.hpp"
#include "config/config_utils_test_support.hpp"
#include "support/file_security.hpp"
#include "test_support.hpp"

#include <clocale>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

namespace howdy::test {
	namespace {
		auto ExpectFloatWritePaths(const std::filesystem::path &config_path) -> bool {
			auto write_float_config = [&] -> bool {
				return WriteConfigTestFile(config_path, "[video]\n"
				                                        "clahe_clip_limit = 2\n"
				                                        "\n"
				                                        "[face]\n"
				                                        "yunet_score_threshold = 0.9\n"
				                                        "yunet_nms_threshold = 0.4\n"
				                                        "sface_threshold = 0.5\n");
			};

			auto const expect_float_write_path = [&](const std::string &label) -> bool {
				bool ok = true;
				ok &= Expect(write_float_config(), label + ": write baseline float config");
				ok &= Expect(howdy::native::UpdateConfigValue(
				                 config_path, {.key = "clahe_clip_limit", .value = "1.25"},
				                 {.lock = false, .validation_root = {config_path.parent_path()}})
				                     .status == howdy::native::ConfigWriteStatus::kOk,
				             label + ": update_config_value accepts clahe_clip_limit dot decimal");
				ok &= Expect(ReadFile(config_path).contains("clahe_clip_limit = 1.25\n"),
				             label + ": clahe_clip_limit written unchanged");
				ok &= Expect(howdy::native::UpdateConfigValue(
				                 config_path, {.key = "yunet_score_threshold", .value = "0.8845"},
				                 {.lock = false, .validation_root = {config_path.parent_path()}})
				                     .status == howdy::native::ConfigWriteStatus::kOk,
				             label +
				                 ": update_config_value accepts yunet_score_threshold dot decimal");
				ok &= Expect(ReadFile(config_path).contains("yunet_score_threshold = 0.8845\n"),
				             label + ": yunet_score_threshold written unchanged");
				ok &=
				    Expect(howdy::native::UpdateConfigValue(
				               config_path, {.key = "yunet_nms_threshold", .value = "0.3"},
				               {.lock = false, .validation_root = {config_path.parent_path()}})
					               .status == howdy::native::ConfigWriteStatus::kOk,
					       label + ": update_config_value accepts yunet_nms_threshold dot decimal");
				ok &= Expect(ReadFile(config_path).contains("yunet_nms_threshold = 0.3\n"),
				             label + ": yunet_nms_threshold written unchanged");
				ok &= Expect(howdy::native::UpdateConfigValue(
				                 config_path, {.key = "sface_threshold", .value = "0.6942"},
				                 {.lock = false, .validation_root = {config_path.parent_path()}})
				                     .status == howdy::native::ConfigWriteStatus::kOk,
				             label + ": update_config_value accepts sface_threshold dot decimal");
				ok &= Expect(ReadFile(config_path).contains("sface_threshold = 0.6942\n"),
				             label + ": sface_threshold written unchanged");

				for (const auto *const value : {"1,25", "1.25abc", "nan", "inf", "+inf", "-inf"}) {
					const auto before_invalid = ReadFile(config_path);
					ok &=
					    Expect(howdy::native::UpdateConfigValue(
					               config_path, {.key = "clahe_clip_limit", .value = value},
					               {.lock = false, .validation_root = {config_path.parent_path()}})
						               .status != howdy::native::ConfigWriteStatus::kOk,
						       label + ": update_config_value rejects invalid float " +
						           std::string(value));
					ok &= Expect(ReadFile(config_path) == before_invalid,
					             label + ": invalid float update leaves config unchanged for " +
					                 std::string(value));
				}
				return ok;
			};

			bool ok = expect_float_write_path("C locale");

			const char *current_locale  = std::setlocale(LC_ALL, nullptr);
			const auto  previous_locale = current_locale == nullptr ? std::optional<std::string>()
			                                                        : std::string(current_locale);
			{
				const ScopedEnvironmentVariable lc_all("LC_ALL");
				const ScopedEnvironmentVariable lang("LANG", "C");
				const ScopedEnvironmentVariable lc_numeric("LC_NUMERIC", "nl_NL.UTF-8");
				if (std::setlocale(LC_ALL, "") != nullptr) {
					ok &= expect_float_write_path("LC_NUMERIC=nl_NL.UTF-8");
				} else {
					std::cerr << "SKIP: nl_NL.UTF-8 locale is not generated\n";
				}
			}
			if (previous_locale.has_value()) {
				ok &= Expect(std::setlocale(LC_ALL, previous_locale->c_str()) != nullptr,
				             "restore original locale");
			}
			return ok;
		}

	}  // namespace

	auto RunConfigReadUpdateTests(ConfigUtilsTestContext &context) -> bool {
		using howdy::test::Expect;
		bool ok = true;
		ok &= Expect(WriteConfigTestFile(context.config_path, "[core]\n"
		                                                      "disabled = false\n"
		                                                      "\n"
		                                                      "[video]\n"
		                                                      "dark_threshold = 60\n"),
		             "write initial config");
		const auto parentless_check = howdy::native::CheckSecureConfigPath(
		    "config.ini", howdy::native::DefaultSecureOwnerUid(), {context.temp_root});
		ok &= Expect(!parentless_check.ok &&
		                 parentless_check.error_message.contains("must have a parent directory"),
		             "check_secure_config_path rejects parentless config path");

		ok &= Expect(WriteConfigTestFile(context.config_path, context.config_at_limit),
		             "write config exactly at size limit");
		// The padding-only fixture has no "disabled" key, so the update must get past the read
		// and the size limit and fail only on the missing option.
		auto size_result = howdy::native::UpdateConfigValue(
		    context.config_path, {.key = "disabled", .value = "true"},
		    {.lock = false, .validate_runtime = false, .validation_root = {context.temp_root}});
		ok &=
		    Expect(size_result.status == howdy::native::ConfigWriteStatus::kInvalidRequest &&
			           size_result.message == "Could not find a \"disabled\" config option to set",
			       "config exactly at size limit is read before the missing option fails: " +
			           size_result.message);
		ok &= Expect(WriteConfigTestFile(context.config_path, context.config_over_limit),
		             "write oversized config");
		size_result = howdy::native::UpdateConfigValue(
		    context.config_path, {.key = "disabled", .value = "true"},
		    {.lock = false, .validate_runtime = false, .validation_root = {context.temp_root}});
		ok &= Expect(size_result.status == howdy::native::ConfigWriteStatus::kReadFailed,
		             "update_config_value rejects oversized current config");
		ok &= Expect(size_result.message == "Failed to read config file",
		             "oversized update reports config read failure");
		ok &= Expect(ReadFile(context.config_path) == context.config_over_limit,
		             "oversized update leaves current config unchanged");
		ok &= Expect(WriteConfigTestFile(context.config_path, "[core]\n"
		                                                      "disabled = false\n"
		                                                      "\n"
		                                                      "[video]\n"
		                                                      "dark_threshold = 60\n"),
		             "restore initial config after size-limit reads");

		ok &= Expect(howdy::native::UpdateConfigValue(
		                 context.config_path, {.key = "disabled", .value = "true"},
		                 {.lock = false, .validation_root = {context.temp_root}})
		                     .status == howdy::native::ConfigWriteStatus::kOk,
		             "update_config_value succeeds for existing key");
		const auto after_disabled = ReadFile(context.config_path);
		ok &= Expect(after_disabled.contains("disabled = true\n"), "disabled key updated");

		constexpr auto expected_scalar_error =
		    "Config values must be single-line scalars and cannot start with [";
		const auto before_unsafe = ReadFile(context.config_path);

		for (const auto &unsafe_value : {
		         std::string(1, '\0'),
		         std::string("\r"),
		         std::string("true\n[video]\ntimeout = 0"),
		         std::string("[video]"),
		     }) {
			const auto result = howdy::native::UpdateConfigValue(
			    context.config_path, {.key = "disabled", .value = unsafe_value},
			    {.lock = false, .validate_runtime = false, .validation_root = {context.temp_root}});
			ok &= Expect(result.status == howdy::native::ConfigWriteStatus::kInvalidRequest,
			             "update_config_value rejects unsafe scalar value");
			ok &= Expect(result.message == expected_scalar_error,
			             "update_config_value returns exact scalar diagnostic");
			ok &= Expect(ReadFile(context.config_path) == before_unsafe,
			             "unsafe scalar update leaves config file unchanged");
		}

		const auto nonexistent_path_result = howdy::native::UpdateConfigValue(
		    context.temp_root / "nonexistent.ini", {.key = "disabled", .value = "[section]"},
		    {.lock = false, .validate_runtime = false, .validation_root = {context.temp_root}});
		ok &= Expect(nonexistent_path_result.status ==
		                 howdy::native::ConfigWriteStatus::kInvalidRequest,
		             "unsafe scalar fails before filesystem/path checks");
		ok &= Expect(nonexistent_path_result.message == expected_scalar_error,
		             "unsafe scalar on nonexistent path returns scalar diagnostic");

		// inih would trim these, or cut them at what it takes for an inline comment.
		for (const std::string altered_value : {
		         " ",
		         " value",
		         "value ",
		         "\tvalue",
		         "value\f",
		         "; value",
		         ";",
		         "/dev/x ;y",
		         "a\t;b",
		         "a\v; b",
		     }) {
			const auto result = howdy::native::UpdateConfigValue(
			    context.config_path, {.key = "device_path", .value = altered_value},
			    {.lock = false, .validate_runtime = false, .validation_root = {context.temp_root}});
			ok &= Expect(
			    result.status == howdy::native::ConfigWriteStatus::kInvalidRequest &&
			        result.message.starts_with("Config values cannot start or end with whitespace"),
			    "value inih would not read back is refused: " + altered_value);
			ok &= Expect(ReadFile(context.config_path) == before_unsafe,
			             "refused value leaves config file unchanged");
		}

		const auto kept_value_path = context.temp_root / "kept-value.ini";
		for (const std::string kept_value : {
		         "a;b",
		         "a;",
		         "a b",
		         "a #b",
		         "a = b",
		         "a: [b]",
		         "\"quoted \"",
		         "#a",
		     }) {
			ok &= Expect(WriteConfigTestFile(kept_value_path, "[video]\ndevice_path = none\n") &&
			                 howdy::native::UpdateConfigValue(
			                     kept_value_path, {.key = "device_path", .value = kept_value},
			                     {
			                         .lock             = false,
			                         .validate_runtime = false,
			                         .validation_root  = {context.temp_root},
			                     })
			                         .status == howdy::native::ConfigWriteStatus::kOk,
			             "value inih reads back is accepted: " + kept_value);
			howdy::native::ConfigReader const reader(ReadFile(kept_value_path));
			ok &= Expect(reader.Ok() && reader.Get("video", "device_path", "") == kept_value,
			             "accepted value reads back unchanged: " + kept_value);
		}

		const auto empty_scalar_path = context.temp_root / "empty-scalar.ini";
		ok &= Expect(WriteConfigTestFile(empty_scalar_path, "[video]\ndevice_path = none\n"),
		             "write config for empty scalar update");
		ok &= Expect(
		    howdy::native::UpdateConfigValue(
		        empty_scalar_path, {.key = "device_path", .value = ""},
		        {.lock = false, .validate_runtime = false, .validation_root = {context.temp_root}})
		            .status == howdy::native::ConfigWriteStatus::kOk,
		    "update_config_value accepts empty scalar value");
		ok &= Expect(ReadFile(empty_scalar_path) == "[video]\ndevice_path = \n",
		             "empty scalar value is written unchanged");

		ok &= Expect(howdy::native::UpdateConfigValue(
		                 context.config_path, {.key = "dark_threshold", .value = "42"},
		                 {.lock = false, .validation_root = {context.temp_root}})
		                     .status == howdy::native::ConfigWriteStatus::kOk,
		             "update_config_value succeeds in later section");
		const auto after_threshold = ReadFile(context.config_path);
		ok &=
		    Expect(after_threshold.contains("dark_threshold = 42\n"), "dark_threshold key updated");

		const auto whitespace_path = context.temp_root / "whitespace.ini";
		ok &= Expect(WriteConfigTestFile(whitespace_path, "   \n\t  "),
		             "write whitespace-only config lines");
		ok &= Expect(
		    howdy::native::UpdateConfigValue(
		        whitespace_path, {.key = "missing_key", .value = "x"},
		        {.lock = false, .validate_runtime = false, .validation_root = {context.temp_root}})
		            .status != howdy::native::ConfigWriteStatus::kOk,
		    "update_config_value ignores whitespace-only lines");
		const auto alternate_syntax_path = context.temp_root / "alternate-syntax.ini";
		ok &= Expect(WriteConfigTestFile(alternate_syntax_path, "[core]\ndisabled true\n"),
		             "write space-separated config option");
		ok &= Expect(
		    howdy::native::UpdateConfigValue(
		        alternate_syntax_path, {.key = "disabled", .value = "false"},
		        {.lock = false, .validate_runtime = false, .validation_root = {context.temp_root}})
		            .status == howdy::native::ConfigWriteStatus::kOk,
		    "update_config_value accepts space-separated option syntax");
		ok &= Expect(ReadFile(alternate_syntax_path) == "[core]\ndisabled = false\n",
		             "space-separated option is normalized");

		const auto mixed_case_path = context.temp_root / "mixed-case.ini";
		ok &= Expect(WriteConfigTestFile(mixed_case_path, "[CORE]\nDISABLED=false\n"),
		             "write mixed-case config identifiers");
		ok &= Expect(
		    howdy::native::UpdateConfigValue(
		        mixed_case_path, {.key = "disabled", .value = "true"},
		        {.lock = false, .validate_runtime = false, .validation_root = {context.temp_root}})
		            .status == howdy::native::ConfigWriteStatus::kOk,
		    "update_config_value matches config identifiers case-insensitively");
		ok &= Expect(ReadFile(mixed_case_path) == "[CORE]\ndisabled = true\n",
		             "mixed-case option is updated in matching section");

		const auto bom_path = context.temp_root / "bom.ini";
		ok &= Expect(WriteConfigTestFile(bom_path, "\xEF\xBB\xBF[core]\ndisabled=false\n"),
		             "write config with UTF-8 BOM");
		ok &= Expect(
		    howdy::native::UpdateConfigValue(
		        bom_path, {.key = "disabled", .value = "true"},
		        {.lock = false, .validate_runtime = false, .validation_root = {context.temp_root}})
		            .status == howdy::native::ConfigWriteStatus::kOk,
		    "update_config_value recognizes BOM-prefixed section");
		ok &= Expect(ReadFile(bom_path) == "\xEF\xBB\xBF[core]\ndisabled = true\n",
		             "BOM-prefixed config option is updated");

		const auto duplicate_name_path = context.temp_root / "duplicate-name.ini";
		ok &= Expect(WriteConfigTestFile(duplicate_name_path, "[other]\n"
		                                                      "disabled = false\n"
		                                                      "\n"
		                                                      "[core]\n"
		                                                      "disabled backup = false\n"
		                                                      "disabled=false\n"),
		             "write same option name in unrelated section");
		ok &= Expect(
		    howdy::native::UpdateConfigValue(
		        duplicate_name_path, {.key = "disabled", .value = "true"},
		        {.lock = false, .validate_runtime = false, .validation_root = {context.temp_root}})
		            .status == howdy::native::ConfigWriteStatus::kOk,
		    "update_config_value targets schema-owned section");
		const auto duplicate_name_content = ReadFile(duplicate_name_path);
		ok &= Expect(duplicate_name_content == "[other]\n"
		                                       "disabled = false\n"
		                                       "\n"
		                                       "[core]\n"
		                                       "disabled backup = false\n"
		                                       "disabled = true\n",
		             "unrelated and similarly named options remain unchanged");
		howdy::native::ConfigReader const duplicate_name_reader(ReadFile(duplicate_name_path));
		ok &= Expect(duplicate_name_reader.Ok() &&
		                 duplicate_name_reader.GetBool("core", "disabled", false),
		             "updated file has effective core.disabled value");

		const auto        duplicate_target_path    = context.temp_root / "duplicate-target.ini";
		const std::string duplicate_target_content = "[core]\n"
		                                             "disabled =\n"
		                                             "disabled = false\n";
		ok &= Expect(WriteConfigTestFile(duplicate_target_path, duplicate_target_content),
		             "write duplicate target option");
		howdy::native::ConfigReader const duplicate_target_reader(ReadFile(duplicate_target_path));
		ok &= Expect(duplicate_target_reader.Ok() &&
		                 !duplicate_target_reader.GetBool("core", "disabled", true),
		             "duplicate target fixture has effective false value before update");
		const auto duplicate_target_result = howdy::native::UpdateConfigValue(
		    duplicate_target_path, {.key = "disabled", .value = "true"},
		    {.lock = false, .validate_runtime = false, .validation_root = {context.temp_root}});
		ok &= Expect(duplicate_target_result.status ==
		                 howdy::native::ConfigWriteStatus::kInvalidRequest,
		             "update_config_value rejects duplicate target options");
		ok &= Expect(duplicate_target_result.message.contains("appears more than once"),
		             "duplicate target update reports duplicate option");
		ok &= Expect(ReadFile(duplicate_target_path) == duplicate_target_content,
		             "rejected duplicate target update leaves config unchanged");

		ok &= Expect(howdy::native::UpdateConfigValue(
		                 context.config_path, {.key = "missing_key", .value = "x"},
		                 {.lock = false, .validation_root = {context.temp_root}})
		                     .status != howdy::native::ConfigWriteStatus::kOk,
		             "update_config_value fails for missing key");
		ok &= Expect(howdy::native::UpdateConfigValue(
		                 context.config_path,
		                 {.key = "dark_threshold", .value = "0\n[core]\ndisabled = true"},
		                 {.lock = false, .validation_root = {context.temp_root}})
		                     .status != howdy::native::ConfigWriteStatus::kOk,
		             "update_config_value rejects newline injection");
		ok &= Expect(ReadFile(context.config_path) == after_threshold,
		             "rejected injection leaves config unchanged");
		ok &= Expect(howdy::native::UpdateConfigValue(
		                 context.config_path, {.key = "dark_threshold", .value = "1000"},
		                 {.lock = false, .validation_root = {context.temp_root}})
		                     .status == howdy::native::ConfigWriteStatus::kInvalidRequest,
		             "update_config_value rejects semantically invalid values as the request");
		ok &= Expect(ReadFile(context.config_path) == after_threshold,
		             "semantic validation failure leaves config unchanged");
		ok &= Expect(howdy::native::UpdateConfigValue(
		                 context.config_path, {.key = "device_fps", .value = "-1"},
		                 {.lock = false, .validation_root = {context.temp_root}})
		                     .status != howdy::native::ConfigWriteStatus::kOk,
		             "update_config_value rejects invalid device_fps values");
		ok &= Expect(ReadFile(context.config_path) == after_threshold,
		             "invalid device_fps update leaves config unchanged");

		ok &= ExpectFloatWritePaths(context.config_path);

		ok &= Expect(WriteConfigTestFile(context.config_path, "[core]\n"
		                                                      "disabled = false\n"
		                                                      "\n"
		                                                      "[video]\n"
		                                                      "timeout = 0\n"),
		             "write config with invalid timeout");
		ok &= Expect(howdy::native::UpdateConfigValue(
		                 context.config_path, {.key = "disabled", .value = "true"},
		                 {.lock = false, .validation_root = {context.temp_root}})
		                     .status == howdy::native::ConfigWriteStatus::kInvalidConfig,
		             "an update that leaves another setting invalid blames the config");
		ok &= Expect(
		    howdy::native::UpdateConfigValue(
		        context.config_path, {.key = "disabled", .value = "true"},
		        {.lock = false, .validate_runtime = false, .validation_root = {context.temp_root}})
		            .status == howdy::native::ConfigWriteStatus::kOk,
		    "update_config_value can bypass runtime validation for recovery writes");
		const auto after_disable_recovery = ReadFile(context.config_path);
		ok &= Expect(after_disable_recovery.contains("disabled = true\n"),
		             "recovery write updates disabled key");
		howdy::native::ConfigReader const still_invalid(after_disable_recovery);
		ok &= Expect(still_invalid.Ok(), "recovery-write config still parses");
		ok &= Expect(howdy::native::ValidateRuntimeConfig(still_invalid).has_value(),
		             "recovery write does not mask unrelated invalid config values");
		ok &= Expect(WriteConfigTestFile(context.config_path, after_threshold),
		             "restore valid config after recovery-write test");

		const auto multiline_path = context.temp_root / "multiline.ini";
		ok &= Expect(WriteConfigTestFile(multiline_path, "[video]\n"
		                                                 "device_path = /dev/video0\n"
		                                                 "  continued_line\n"
		                                                 "timeout = 4\n"),
		             "write multiline INI option fixture");
		ok &= Expect(howdy::native::UpdateConfigValue(
		                 multiline_path, {.key = "device_path", .value = "none"},
		                 {.lock = false, .validation_root = {context.temp_root}})
		                     .status == howdy::native::ConfigWriteStatus::kOk,
		             "update_config_value replaces multiline INI option");
		ok &= Expect(ReadFile(multiline_path) == "[video]\n"
		                                         "device_path = none\n"
		                                         "timeout = 4\n",
		             "multiline INI update removes orphaned continuation lines");

		ok &= Expect(WriteConfigTestFile(multiline_path, "[video]\n"
		                                                 "device_path = /dev/video0\n"
		                                                 "\n"
		                                                 "; comment\n"
		                                                 "  stale\n"
		                                                 "\n"
		                                                 "timeout = 4\n"),
		             "write continuation-after-blank-and-comment fixture");
		ok &= Expect(howdy::native::UpdateConfigValue(
		                 multiline_path, {.key = "device_path", .value = "none"},
		                 {.lock = false, .validation_root = {context.temp_root}})
		                     .status == howdy::native::ConfigWriteStatus::kOk,
		             "update_config_value replaces value continued across blank/comment lines");
		const auto after_gap_update = ReadFile(multiline_path);
		ok &= Expect(after_gap_update == "[video]\n"
		                                 "device_path = none\n"
		                                 "\n"
		                                 "; comment\n"
		                                 "\n"
		                                 "timeout = 4\n",
		             "continuation after blank/comment lines is removed; comments are kept");
		howdy::native::ConfigReader const gap_reader(after_gap_update);
		ok &= Expect(gap_reader.Ok() && gap_reader.Get("video", "device_path", "") == "none",
		             "INI parser reads the replaced value without a stale continuation");

		ok &= Expect(
		    WriteConfigTestFile(multiline_path,
			                    "[video]\n"
			                    "device_path =\n"
			                    "  /dev/v4l/by-path/pci-0000:00:14.0-usb-0:1:1.0-video-index0\n"
			                    "timeout = 4\n"),
		    "write device path continuation fixture");
		ok &= Expect(howdy::native::UpdateConfigValue(
		                 multiline_path, {.key = "device_path", .value = "none"},
		                 {.lock = false, .validation_root = {context.temp_root}})
		                         .status == howdy::native::ConfigWriteStatus::kOk &&
		                 ReadFile(multiline_path) == "[video]\n"
		                                             "device_path = none\n"
		                                             "timeout = 4\n",
		             "a continued device path with colons is replaced as a plain value");

		ok &= Expect(WriteConfigTestFile(multiline_path, "[video]\n"
		                                                 "device_path = /dev/video0\n"
		                                                 "\f\n"
		                                                 "\f  /dev/video1\n"
		                                                 "\v/dev/video2\n"
		                                                 "timeout = 4\n"),
		             "write form-feed continuation fixture");
		ok &=
		    Expect(howdy::native::UpdateConfigValue(
		               multiline_path, {.key = "device_path", .value = "none"},
		               {.lock = false, .validation_root = {context.temp_root}})
			                   .status == howdy::native::ConfigWriteStatus::kOk &&
			           ReadFile(multiline_path) == "[video]\n"
			                                       "device_path = none\n"
			                                       "\f\n"
			                                       "timeout = 4\n",
			       "continuations indented with other whitespace are replaced as inih reads them");

		ok &= Expect(WriteConfigTestFile(multiline_path, "[video]\n"
		                                                 "device_path = /dev/video0\n"
		                                                 "  second ; note: old = camera\n"
		                                                 "timeout = 4\n"),
		             "write inline-comment continuation fixture");
		ok &= Expect(howdy::native::UpdateConfigValue(
		                 multiline_path, {.key = "device_path", .value = "none"},
		                 {.lock = false, .validation_root = {context.temp_root}})
		                         .status == howdy::native::ConfigWriteStatus::kOk &&
		                 ReadFile(multiline_path) == "[video]\n"
		                                             "device_path = none\n"
		                                             "timeout = 4\n",
		             "separators in an inline comment do not make a continuation ambiguous");

		for (const auto *const bare :
		     {"disabled\r\n", "disabled\ftrue\n", "disabled\n", "disabled"}) {
			ok &= Expect(WriteConfigTestFile(multiline_path, std::string("[core]\n") + bare),
			             "write separator-less option fixture");
			ok &= Expect(howdy::native::UpdateConfigValue(
			                 multiline_path, {.key = "disabled", .value = "true"},
			                 {.lock = false, .validation_root = {context.temp_root}})
			                         .status == howdy::native::ConfigWriteStatus::kOk &&
			                 ReadFile(multiline_path) == "[core]\ndisabled = true\n",
			             "separator-less option line is repaired whatever whitespace ends it");
		}

		// Each indented line continues the first value, but erasing it would drop a setting.
		for (const auto *const ambiguous : {
		         "[core]\ndisabled = false\n   \n  disabled = true\n",
		         "[core]\n  disabled = false\n  abort_if_ssh = true\n",
		         "[core]\ndisabled = false\n; note\n  abort_if_ssh: true\n",
		         "[core]\ndisabled = false\n\n  [video]\n",
		         "[core]\ndisabled = false\n  abort_if_ssh true\n",
		         "[core]\ndisabled = false\n  abort-if-ssh = true\n",
		         "[core]\ndisabled = false\n  abort.if.ssh = true\n",
		         "[core]\ndisabled = false\n  [video\n",
		         "[core]\ndisabled = false\n  abort if ssh = true\n",
		         "[core]\ndisabled = false\n  \"abort_if_ssh\" = true\n",
		         "[core]\ndisabled = false\n  abort_if_ssh\n",
		         "[core]\ndisabled = false\n  abort_if_ssh",
		         // A line without a separator owns nothing: what follows belongs to the option
		         // above.
		         "[core]\nabort_if_ssh = true\ndisabled\n  second\n",
		         "[core]\nabort_if_ssh = true\ndisabled false\nmalformed\n  second\n",
		         "[core]\ndisabled\n  abort_if_ssh = true\n",
		     }) {
			ok &= Expect(WriteConfigTestFile(multiline_path, ambiguous),
			             "write ambiguous continuation fixture");
			const auto ambiguous_result = howdy::native::UpdateConfigValue(
			    multiline_path, {.key = "disabled", .value = "true"},
			    {.lock = false, .validate_runtime = false, .validation_root = {context.temp_root}});
			ok &= Expect(ambiguous_result.status ==
			                     howdy::native::ConfigWriteStatus::kInvalidRequest &&
			                 ambiguous_result.message.starts_with("Config line ") &&
			                 ambiguous_result.message.contains(" is indented"),
			             "ambiguous continuation is refused by line: " + ambiguous_result.message);
			ok &= Expect(ReadFile(multiline_path) == ambiguous,
			             "refused ambiguous continuation leaves the config unchanged");
		}

		return ok;
	}

}  // namespace howdy::test
