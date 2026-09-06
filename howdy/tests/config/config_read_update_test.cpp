#include "config/test_hooks.hpp"
#include "config/config_utils.hpp"
#include "config/config_utils_test_support.hpp"
#include "config/config_validation.hpp"
#include "test_support.hpp"

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

namespace howdy::test {
	namespace {
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
				return write_config_test_file(config_path, "[video]\n"
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
				ok &= expect(
				    howdy::native::update_config_value(config_path, "clahe_clip_limit", "1.25"),
				    label + ": update_config_value accepts clahe_clip_limit dot decimal");
				ok &=
				    expect(read_config_test_file(config_path).contains("clahe_clip_limit = 1.25\n"),
				           label + ": clahe_clip_limit written unchanged");
				ok &= expect(howdy::native::update_config_value(config_path,
				                                                "yunet_score_threshold", "0.8845"),
				             label +
				                 ": update_config_value accepts yunet_score_threshold dot decimal");
				ok &= expect(
				    read_config_test_file(config_path).contains("yunet_score_threshold = 0.8845\n"),
				    label + ": yunet_score_threshold written unchanged");
				ok &= expect(
				    howdy::native::update_config_value(config_path, "yunet_nms_threshold", "0.3"),
				    label + ": update_config_value accepts yunet_nms_threshold dot decimal");
				ok &= expect(
				    read_config_test_file(config_path).contains("yunet_nms_threshold = 0.3\n"),
				    label + ": yunet_nms_threshold written unchanged");
				ok &= expect(
				    howdy::native::update_config_value(config_path, "sface_threshold", "0.6942"),
				    label + ": update_config_value accepts sface_threshold dot decimal");
				ok &= expect(
				    read_config_test_file(config_path).contains("sface_threshold = 0.6942\n"),
				    label + ": sface_threshold written unchanged");

				for (const auto *const value : {"1,25", "1.25abc", "nan", "inf", "+inf", "-inf"}) {
					const auto before_invalid = read_config_test_file(config_path);
					ok &= expect(
					    !howdy::native::update_config_value(config_path, "clahe_clip_limit", value),
					    label + ": update_config_value rejects invalid float " +
					        std::string(value));
					ok &= expect(read_config_test_file(config_path) == before_invalid,
					             label + ": invalid float update leaves config unchanged for " +
					                 std::string(value));
				}
				return ok;
			};

			bool ok = expect_float_write_path("C locale");

			const char *current_locale  = std::setlocale(LC_ALL, nullptr);
			const auto  previous_locale = current_locale == nullptr ? std::optional<std::string>()
			                                                        : std::string(current_locale);
			const auto  previous_lc_all = get_env_value("LC_ALL");
			const auto  previous_lc_numeric = get_env_value("LC_NUMERIC");
			const auto  previous_lang       = get_env_value("LANG");
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

	}  // namespace

	auto run_config_read_update_tests(ConfigUtilsTestContext &context) -> bool {
		namespace fs = std::filesystem;
		using howdy::test::expect;
		bool ok = true;
		ok &= expect(write_config_test_file(context.config_path, "[core]\n"
		                                                         "disabled = false\n"
		                                                         "\n"
		                                                         "[video]\n"
		                                                         "dark_threshold = 60\n"),
		             "write initial config");
		const auto parentless_check = howdy::native::check_secure_config_path("config.ini");
		ok &= expect(!parentless_check.ok &&
		                 parentless_check.error_message.contains("must have a parent directory"),
		             "check_secure_config_path rejects parentless config path");

		const auto lines = howdy::native::read_config_lines(context.config_path, false);
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

			const auto read_lines = howdy::native::read_config_lines(context.config_path, false);
			ok &= expect(replacement_ok, "read_config_lines pathname replacement succeeded");
			ok &= expect(read_lines.size() == 5 && read_lines[0] == "[core]\n",
			             "read_config_lines reads from opened descriptor, not replaced pathname");
			std::error_code ec;
			fs::remove(context.config_path, ec);
			fs::remove(backup_path, ec);
			ok &= expect(write_config_test_file(context.config_path, "[core]\n"
			                                                         "disabled = false\n"
			                                                         "\n"
			                                                         "[video]\n"
			                                                         "dark_threshold = 60\n"),
			             "restore baseline config after hook test");
		}

		ok &= expect(write_config_test_file(context.config_path, context.config_at_limit),
		             "write config exactly at size limit");
		const auto limit_lines = howdy::native::read_config_lines(context.config_path, false);
		ok &= expect(limit_lines.size() == 1 && limit_lines.front() == context.config_at_limit,
		             "read_config_lines accepts config exactly at size limit");
		ok &= expect(write_config_test_file(context.config_path, context.config_over_limit),
		             "write oversized config");
		ok &= expect(howdy::native::read_config_lines(context.config_path, false).empty(),
		             "read_config_lines rejects oversized config");
		std::string size_error;
		ok &= expect(!howdy::native::update_config_value(context.config_path, "disabled",
		                                                 &size_error, "true", false, false),
		             "update_config_value rejects oversized current config");
		ok &= expect(size_error == "Failed to read config file",
		             "oversized update reports config read failure");
		ok &= expect(read_config_test_file(context.config_path) == context.config_over_limit,
		             "oversized update leaves current config unchanged");
		ok &= expect(write_config_test_file(context.config_path, "[core]\n"
		                                                         "disabled = false\n"
		                                                         "\n"
		                                                         "[video]\n"
		                                                         "dark_threshold = 60\n"),
		             "restore initial config after size-limit reads");

		ok &= expect(howdy::native::update_config_value(context.config_path, "disabled", "true"),
		             "update_config_value succeeds for existing key");
		const auto after_disabled = read_config_test_file(context.config_path);
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

		ok &=
		    expect(howdy::native::update_config_value(context.config_path, "dark_threshold", "42"),
		           "update_config_value succeeds in later section");
		const auto after_threshold = read_config_test_file(context.config_path);
		ok &=
		    expect(after_threshold.contains("dark_threshold = 42\n"), "dark_threshold key updated");

		const auto whitespace_path = context.temp_root / "whitespace.ini";
		ok &= expect(write_config_test_file(whitespace_path, "   \n\t  "),
		             "write whitespace-only config lines");
		ok &= expect(!howdy::native::update_config_value(whitespace_path, "missing_key", nullptr,
		                                                 "x", false, false),
		             "update_config_value ignores whitespace-only lines");
		const auto alternate_syntax_path = context.temp_root / "alternate-syntax.ini";
		ok &= expect(write_config_test_file(alternate_syntax_path, "[core]\ndisabled true\n"),
		             "write space-separated config option");
		ok &= expect(howdy::native::update_config_value(alternate_syntax_path, "disabled", nullptr,
		                                                "false", false, false),
		             "update_config_value accepts space-separated option syntax");
		ok &= expect(read_config_test_file(alternate_syntax_path) == "[core]\ndisabled = false\n",
		             "space-separated option is normalized");

		const auto mixed_case_path = context.temp_root / "mixed-case.ini";
		ok &= expect(write_config_test_file(mixed_case_path, "[CORE]\nDISABLED=false\n"),
		             "write mixed-case config identifiers");
		ok &= expect(howdy::native::update_config_value(mixed_case_path, "disabled", nullptr,
		                                                "true", false, false),
		             "update_config_value matches config identifiers case-insensitively");
		ok &= expect(read_config_test_file(mixed_case_path) == "[CORE]\ndisabled = true\n",
		             "mixed-case option is updated in matching section");

		const auto bom_path = context.temp_root / "bom.ini";
		ok &= expect(write_config_test_file(bom_path, "\xEF\xBB\xBF[core]\ndisabled=false\n"),
		             "write config with UTF-8 BOM");
		ok &= expect(
		    howdy::native::update_config_value(bom_path, "disabled", nullptr, "true", false, false),
		    "update_config_value recognizes BOM-prefixed section");
		ok &= expect(read_config_test_file(bom_path) == "\xEF\xBB\xBF[core]\ndisabled = true\n",
		             "BOM-prefixed config option is updated");

		const auto duplicate_name_path = context.temp_root / "duplicate-name.ini";
		ok &= expect(write_config_test_file(duplicate_name_path, "[other]\n"
		                                                         "disabled = false\n"
		                                                         "\n"
		                                                         "[core]\n"
		                                                         "disabled backup = false\n"
		                                                         "disabled=false\n"),
		             "write same option name in unrelated section");
		ok &= expect(howdy::native::update_config_value(duplicate_name_path, "disabled", nullptr,
		                                                "true", false, false),
		             "update_config_value targets schema-owned section");
		const auto duplicate_name_content = read_config_test_file(duplicate_name_path);
		ok &= expect(duplicate_name_content == "[other]\n"
		                                       "disabled = false\n"
		                                       "\n"
		                                       "[core]\n"
		                                       "disabled backup = false\n"
		                                       "disabled = true\n",
		             "unrelated and similarly named options remain unchanged");
		howdy::native::ConfigReader duplicate_name_reader(duplicate_name_path.string());
		ok &= expect(duplicate_name_reader.ok() &&
		                 duplicate_name_reader.get_bool("core", "disabled", false),
		             "updated file has effective core.disabled value");

		const auto        duplicate_target_path    = context.temp_root / "duplicate-target.ini";
		const std::string duplicate_target_content = "[core]\n"
		                                             "disabled =\n"
		                                             "disabled = false\n";
		ok &= expect(write_config_test_file(duplicate_target_path, duplicate_target_content),
		             "write duplicate target option");
		howdy::native::ConfigReader duplicate_target_reader(duplicate_target_path.string());
		ok &= expect(duplicate_target_reader.ok() &&
		                 !duplicate_target_reader.get_bool("core", "disabled", true),
		             "duplicate target fixture has effective false value before update");
		std::string duplicate_target_error;
		ok &= expect(!howdy::native::update_config_value(duplicate_target_path, "disabled",
		                                                 &duplicate_target_error, "true", false,
		                                                 false),
		             "update_config_value rejects duplicate target options");
		ok &= expect(duplicate_target_error.contains("appears more than once"),
		             "duplicate target update reports duplicate option");
		ok &= expect(read_config_test_file(duplicate_target_path) == duplicate_target_content,
		             "rejected duplicate target update leaves config unchanged");

		ok &= expect(!howdy::native::update_config_value(context.config_path, "missing_key", "x"),
		             "update_config_value fails for missing key");
		ok &= expect(!howdy::native::update_config_value(context.config_path, "dark_threshold",
		                                                 "0\n[core]\ndisabled = true"),
		             "update_config_value rejects newline injection");
		ok &= expect(read_config_test_file(context.config_path) == after_threshold,
		             "rejected injection leaves config unchanged");
		ok &= expect(
		    !howdy::native::update_config_value(context.config_path, "dark_threshold", "1000"),
		    "update_config_value rejects semantically invalid values");
		ok &= expect(read_config_test_file(context.config_path) == after_threshold,
		             "semantic validation failure leaves config unchanged");
		ok &= expect(!howdy::native::update_config_value(context.config_path, "device_fps", "-1"),
		             "update_config_value rejects invalid device_fps values");
		ok &= expect(read_config_test_file(context.config_path) == after_threshold,
		             "invalid device_fps update leaves config unchanged");

		ok &= expect_float_write_paths(context.config_path);

		ok &= expect(write_config_test_file(context.config_path, "[core]\n"
		                                                         "disabled = false\n"
		                                                         "\n"
		                                                         "[video]\n"
		                                                         "timeout = 0\n"),
		             "write config with invalid timeout");
		ok &= expect(howdy::native::update_config_value(context.config_path, "disabled", nullptr,
		                                                "true", false, false),
		             "update_config_value can bypass runtime validation for recovery writes");
		const auto after_disable_recovery = read_config_test_file(context.config_path);
		ok &= expect(after_disable_recovery.contains("disabled = true\n"),
		             "recovery write updates disabled key");
		howdy::native::ConfigReader still_invalid(context.config_path.string());
		ok &= expect(still_invalid.ok(), "recovery-write config still parses");
		ok &= expect(howdy::native::validate_runtime_config(still_invalid).has_value(),
		             "recovery write does not mask unrelated invalid config values");
		ok &= expect(write_config_test_file(context.config_path, after_threshold),
		             "restore valid config after recovery-write test");

		return ok;
	}

}  // namespace howdy::test
