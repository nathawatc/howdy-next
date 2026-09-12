#include "config/config_utils.hpp"
#include "config/config_utils_test_support.hpp"
#include "config/config_validation.hpp"
#include "config/test_hooks.hpp"
#include "test_support.hpp"

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

namespace howdy::test {
	namespace {
		auto GetEnvValue(const char *name) -> std::optional<std::string> {
			const char *value = std::getenv(name);
			if (value == nullptr) {
				return std::nullopt;
			}
			return std::string(value);
		}

		void RestoreEnvValue(const char *name, const std::optional<std::string> &value) {
			if (value.has_value()) {
				setenv(name, value->c_str(), 1);
				return;
			}
			unsetenv(name);
		}

		auto ExpectFloatWritePaths(const std::filesystem::path &config_path) -> bool {
			auto write_float_config = [&]() -> bool {
				return WriteConfigTestFile(config_path, "[video]\n"
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
				ok &= expect(howdy::native::UpdateConfigValue(config_path, "clahe_clip_limit",
				                                              nullptr, "1.25", false, true,
				                                              {config_path.parent_path()}),
				             label + ": update_config_value accepts clahe_clip_limit dot decimal");
				ok &= expect(ReadConfigTestFile(config_path).contains("clahe_clip_limit = 1.25\n"),
				             label + ": clahe_clip_limit written unchanged");
				ok &= expect(howdy::native::UpdateConfigValue(config_path, "yunet_score_threshold",
				                                              nullptr, "0.8845", false, true,
				                                              {config_path.parent_path()}),
				             label +
				                 ": update_config_value accepts yunet_score_threshold dot decimal");
				ok &= expect(
				    ReadConfigTestFile(config_path).contains("yunet_score_threshold = 0.8845\n"),
				    label + ": yunet_score_threshold written unchanged");
				ok &=
				    expect(howdy::native::UpdateConfigValue(config_path, "yunet_nms_threshold",
				                                            nullptr, "0.3", false, true,
				                                            {config_path.parent_path()}),
				           label + ": update_config_value accepts yunet_nms_threshold dot decimal");
				ok &=
				    expect(ReadConfigTestFile(config_path).contains("yunet_nms_threshold = 0.3\n"),
				           label + ": yunet_nms_threshold written unchanged");
				ok &= expect(howdy::native::UpdateConfigValue(config_path, "sface_threshold",
				                                              nullptr, "0.6942", false, true,
				                                              {config_path.parent_path()}),
				             label + ": update_config_value accepts sface_threshold dot decimal");
				ok &= expect(ReadConfigTestFile(config_path).contains("sface_threshold = 0.6942\n"),
				             label + ": sface_threshold written unchanged");

				for (const auto *const value : {"1,25", "1.25abc", "nan", "inf", "+inf", "-inf"}) {
					const auto before_invalid = ReadConfigTestFile(config_path);
					ok &= expect(!howdy::native::UpdateConfigValue(config_path, "clahe_clip_limit",
					                                               nullptr, value, false, true,
					                                               {config_path.parent_path()}),
					             label + ": update_config_value rejects invalid float " +
					                 std::string(value));
					ok &= expect(ReadConfigTestFile(config_path) == before_invalid,
					             label + ": invalid float update leaves config unchanged for " +
					                 std::string(value));
				}
				return ok;
			};

			bool ok = expect_float_write_path("C locale");

			const char *current_locale  = std::setlocale(LC_ALL, nullptr);
			const auto  previous_locale = current_locale == nullptr ? std::optional<std::string>()
			                                                        : std::string(current_locale);
			const auto  previous_lc_all = GetEnvValue("LC_ALL");
			const auto  previous_lc_numeric = GetEnvValue("LC_NUMERIC");
			const auto  previous_lang       = GetEnvValue("LANG");
			unsetenv("LC_ALL");
			setenv("LANG", "C", 1);
			setenv("LC_NUMERIC", "nl_NL.UTF-8", 1);
			if (std::setlocale(LC_ALL, "") != nullptr) {
				ok &= expect_float_write_path("LC_NUMERIC=nl_NL.UTF-8");
			} else {
				std::cerr << "SKIP: nl_NL.UTF-8 locale is not generated\n";
			}
			RestoreEnvValue("LC_ALL", previous_lc_all);
			RestoreEnvValue("LC_NUMERIC", previous_lc_numeric);
			RestoreEnvValue("LANG", previous_lang);
			if (previous_locale.has_value()) {
				std::setlocale(LC_ALL, previous_locale->c_str());
			}
			return ok;
		}

	}  // namespace

	auto RunConfigReadUpdateTests(ConfigUtilsTestContext &context) -> bool {
		namespace fs = std::filesystem;
		using howdy::test::expect;
		bool ok = true;
		ok &= expect(WriteConfigTestFile(context.config_path, "[core]\n"
		                                                      "disabled = false\n"
		                                                      "\n"
		                                                      "[video]\n"
		                                                      "dark_threshold = 60\n"),
		             "write initial config");
		const auto parentless_check = howdy::native::CheckSecureConfigPath(
		    "config.ini", howdy::native::DefaultSecureOwnerUid(), {context.temp_root});
		ok &= expect(!parentless_check.ok &&
		                 parentless_check.error_message.contains("must have a parent directory"),
		             "check_secure_config_path rejects parentless config path");

		const auto lines =
		    howdy::native::ReadConfigLines(context.config_path, false, {context.temp_root});
		ok &= expect(lines.size() == 5, "read_config_lines returns expected line count");
		ok &= expect(!lines.empty() && lines[0] == "[core]\n",
		             "read_config_lines preserves newlines");

		{
			bool       replacement_ok = false;
			const auto backup_path    = context.temp_root / "backup-read-lines.ini";
			const howdy::native::config_test_hooks::ScopedHooks hooks([&]() -> void {
				replacement_ok = rename(context.config_path.c_str(), backup_path.c_str()) == 0 &&
				                 symlink("/dev/null", context.config_path.c_str()) == 0;
			});

			const auto read_lines =
			    howdy::native::ReadConfigLines(context.config_path, false, {context.temp_root});
			ok &= expect(replacement_ok, "read_config_lines pathname replacement succeeded");
			ok &= expect(read_lines.size() == 5 && read_lines[0] == "[core]\n",
			             "read_config_lines reads from opened descriptor, not replaced pathname");
			std::error_code ec;
			fs::remove(context.config_path, ec);
			fs::remove(backup_path, ec);
			ok &= expect(WriteConfigTestFile(context.config_path, "[core]\n"
			                                                      "disabled = false\n"
			                                                      "\n"
			                                                      "[video]\n"
			                                                      "dark_threshold = 60\n"),
			             "restore baseline config after hook test");
		}

		ok &= expect(WriteConfigTestFile(context.config_path, context.config_at_limit),
		             "write config exactly at size limit");
		const auto limit_lines =
		    howdy::native::ReadConfigLines(context.config_path, false, {context.temp_root});
		ok &= expect(limit_lines.size() == 1 && limit_lines.front() == context.config_at_limit,
		             "read_config_lines accepts config exactly at size limit");
		ok &= expect(WriteConfigTestFile(context.config_path, context.config_over_limit),
		             "write oversized config");
		ok &= expect(
		    howdy::native::ReadConfigLines(context.config_path, false, {context.temp_root}).empty(),
		    "read_config_lines rejects oversized config");
		std::string size_error;
		ok &= expect(!howdy::native::UpdateConfigValue(context.config_path, "disabled", &size_error,
		                                               "true", false, false, {context.temp_root}),
		             "update_config_value rejects oversized current config");
		ok &= expect(size_error == "Failed to read config file",
		             "oversized update reports config read failure");
		ok &= expect(ReadConfigTestFile(context.config_path) == context.config_over_limit,
		             "oversized update leaves current config unchanged");
		ok &= expect(WriteConfigTestFile(context.config_path, "[core]\n"
		                                                      "disabled = false\n"
		                                                      "\n"
		                                                      "[video]\n"
		                                                      "dark_threshold = 60\n"),
		             "restore initial config after size-limit reads");

		ok &= expect(howdy::native::UpdateConfigValue(context.config_path, "disabled", nullptr,
		                                              "true", false, true, {context.temp_root}),
		             "update_config_value succeeds for existing key");
		const auto after_disabled = ReadConfigTestFile(context.config_path);
		ok &= expect(after_disabled.contains("disabled = true\n"), "disabled key updated");

		ok &= expect(howdy::native::IsSafeIniScalarValue("true"),
		             "is_safe_ini_scalar_value accepts simple scalar");
		ok &= expect(howdy::native::IsSafeIniScalarValue(""),
		             "is_safe_ini_scalar_value accepts empty scalar");
		ok &= expect(!howdy::native::IsSafeIniScalarValue(std::string(1, '\0')),
		             "is_safe_ini_scalar_value rejects embedded NUL");
		ok &= expect(!howdy::native::IsSafeIniScalarValue("\r"),
		             "is_safe_ini_scalar_value rejects carriage return");
		ok &= expect(!howdy::native::IsSafeIniScalarValue("true\n[video]\ntimeout = 0"),
		             "is_safe_ini_scalar_value rejects newline injection");
		ok &= expect(!howdy::native::IsSafeIniScalarValue("[video]"),
		             "is_safe_ini_scalar_value rejects section-like values");

		ok &=
		    expect(howdy::native::UpdateConfigValue(context.config_path, "dark_threshold", nullptr,
		                                            "42", false, true, {context.temp_root}),
		           "update_config_value succeeds in later section");
		const auto after_threshold = ReadConfigTestFile(context.config_path);
		ok &=
		    expect(after_threshold.contains("dark_threshold = 42\n"), "dark_threshold key updated");

		const auto whitespace_path = context.temp_root / "whitespace.ini";
		ok &= expect(WriteConfigTestFile(whitespace_path, "   \n\t  "),
		             "write whitespace-only config lines");
		ok &= expect(!howdy::native::UpdateConfigValue(whitespace_path, "missing_key", nullptr, "x",
		                                               false, false, {context.temp_root}),
		             "update_config_value ignores whitespace-only lines");
		const auto alternate_syntax_path = context.temp_root / "alternate-syntax.ini";
		ok &= expect(WriteConfigTestFile(alternate_syntax_path, "[core]\ndisabled true\n"),
		             "write space-separated config option");
		ok &= expect(howdy::native::UpdateConfigValue(alternate_syntax_path, "disabled", nullptr,
		                                              "false", false, false, {context.temp_root}),
		             "update_config_value accepts space-separated option syntax");
		ok &= expect(ReadConfigTestFile(alternate_syntax_path) == "[core]\ndisabled = false\n",
		             "space-separated option is normalized");

		const auto mixed_case_path = context.temp_root / "mixed-case.ini";
		ok &= expect(WriteConfigTestFile(mixed_case_path, "[CORE]\nDISABLED=false\n"),
		             "write mixed-case config identifiers");
		ok &= expect(howdy::native::UpdateConfigValue(mixed_case_path, "disabled", nullptr, "true",
		                                              false, false, {context.temp_root}),
		             "update_config_value matches config identifiers case-insensitively");
		ok &= expect(ReadConfigTestFile(mixed_case_path) == "[CORE]\ndisabled = true\n",
		             "mixed-case option is updated in matching section");

		const auto bom_path = context.temp_root / "bom.ini";
		ok &= expect(WriteConfigTestFile(bom_path, "\xEF\xBB\xBF[core]\ndisabled=false\n"),
		             "write config with UTF-8 BOM");
		ok &= expect(howdy::native::UpdateConfigValue(bom_path, "disabled", nullptr, "true", false,
		                                              false, {context.temp_root}),
		             "update_config_value recognizes BOM-prefixed section");
		ok &= expect(ReadConfigTestFile(bom_path) == "\xEF\xBB\xBF[core]\ndisabled = true\n",
		             "BOM-prefixed config option is updated");

		const auto duplicate_name_path = context.temp_root / "duplicate-name.ini";
		ok &= expect(WriteConfigTestFile(duplicate_name_path, "[other]\n"
		                                                      "disabled = false\n"
		                                                      "\n"
		                                                      "[core]\n"
		                                                      "disabled backup = false\n"
		                                                      "disabled=false\n"),
		             "write same option name in unrelated section");
		ok &= expect(howdy::native::UpdateConfigValue(duplicate_name_path, "disabled", nullptr,
		                                              "true", false, false, {context.temp_root}),
		             "update_config_value targets schema-owned section");
		const auto duplicate_name_content = ReadConfigTestFile(duplicate_name_path);
		ok &= expect(duplicate_name_content == "[other]\n"
		                                       "disabled = false\n"
		                                       "\n"
		                                       "[core]\n"
		                                       "disabled backup = false\n"
		                                       "disabled = true\n",
		             "unrelated and similarly named options remain unchanged");
		howdy::native::ConfigReader duplicate_name_reader(duplicate_name_path.string());
		ok &= expect(duplicate_name_reader.Ok() &&
		                 duplicate_name_reader.GetBool("core", "disabled", false),
		             "updated file has effective core.disabled value");

		const auto        duplicate_target_path    = context.temp_root / "duplicate-target.ini";
		const std::string duplicate_target_content = "[core]\n"
		                                             "disabled =\n"
		                                             "disabled = false\n";
		ok &= expect(WriteConfigTestFile(duplicate_target_path, duplicate_target_content),
		             "write duplicate target option");
		howdy::native::ConfigReader duplicate_target_reader(duplicate_target_path.string());
		ok &= expect(duplicate_target_reader.Ok() &&
		                 !duplicate_target_reader.GetBool("core", "disabled", true),
		             "duplicate target fixture has effective false value before update");
		std::string duplicate_target_error;
		ok &= expect(!howdy::native::UpdateConfigValue(duplicate_target_path, "disabled",
		                                               &duplicate_target_error, "true", false,
		                                               false, {context.temp_root}),
		             "update_config_value rejects duplicate target options");
		ok &= expect(duplicate_target_error.contains("appears more than once"),
		             "duplicate target update reports duplicate option");
		ok &= expect(ReadConfigTestFile(duplicate_target_path) == duplicate_target_content,
		             "rejected duplicate target update leaves config unchanged");

		ok &= expect(!howdy::native::UpdateConfigValue(context.config_path, "missing_key", nullptr,
		                                               "x", false, true, {context.temp_root}),
		             "update_config_value fails for missing key");
		ok &= expect(!howdy::native::UpdateConfigValue(context.config_path, "dark_threshold",
		                                               nullptr, "0\n[core]\ndisabled = true", false,
		                                               true, {context.temp_root}),
		             "update_config_value rejects newline injection");
		ok &= expect(ReadConfigTestFile(context.config_path) == after_threshold,
		             "rejected injection leaves config unchanged");
		ok &=
		    expect(!howdy::native::UpdateConfigValue(context.config_path, "dark_threshold", nullptr,
		                                             "1000", false, true, {context.temp_root}),
		           "update_config_value rejects semantically invalid values");
		ok &= expect(ReadConfigTestFile(context.config_path) == after_threshold,
		             "semantic validation failure leaves config unchanged");
		ok &= expect(!howdy::native::UpdateConfigValue(context.config_path, "device_fps", nullptr,
		                                               "-1", false, true, {context.temp_root}),
		             "update_config_value rejects invalid device_fps values");
		ok &= expect(ReadConfigTestFile(context.config_path) == after_threshold,
		             "invalid device_fps update leaves config unchanged");

		ok &= ExpectFloatWritePaths(context.config_path);

		ok &= expect(WriteConfigTestFile(context.config_path, "[core]\n"
		                                                      "disabled = false\n"
		                                                      "\n"
		                                                      "[video]\n"
		                                                      "timeout = 0\n"),
		             "write config with invalid timeout");
		ok &= expect(howdy::native::UpdateConfigValue(context.config_path, "disabled", nullptr,
		                                              "true", false, false, {context.temp_root}),
		             "update_config_value can bypass runtime validation for recovery writes");
		const auto after_disable_recovery = ReadConfigTestFile(context.config_path);
		ok &= expect(after_disable_recovery.contains("disabled = true\n"),
		             "recovery write updates disabled key");
		howdy::native::ConfigReader still_invalid(context.config_path.string());
		ok &= expect(still_invalid.Ok(), "recovery-write config still parses");
		ok &= expect(howdy::native::ValidateRuntimeConfig(still_invalid).has_value(),
		             "recovery write does not mask unrelated invalid config values");
		ok &= expect(WriteConfigTestFile(context.config_path, after_threshold),
		             "restore valid config after recovery-write test");

		return ok;
	}

}  // namespace howdy::test
