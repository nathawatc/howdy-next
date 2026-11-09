#include "../../src/config/config_document/internal.hpp"
#include "../../src/config/config_validation/internal.hpp"
#include "config/config_document_test_support.hpp"
#include "config/config_utils.hpp"
#include "config/config_utils_test_support.hpp"
#include "support/file_security.hpp"
#include "test_support.hpp"

#include <clocale>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <utility>

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
				ok &= Expect(howdy::native::UpdateConfigValue(config_path, "clahe_clip_limit",
				                                              nullptr, "1.25", false, true,
				                                              {config_path.parent_path()}),
				             label + ": update_config_value accepts clahe_clip_limit dot decimal");
				ok &= Expect(ReadConfigTestFile(config_path).contains("clahe_clip_limit = 1.25\n"),
				             label + ": clahe_clip_limit written unchanged");
				ok &= Expect(howdy::native::UpdateConfigValue(config_path, "yunet_score_threshold",
				                                              nullptr, "0.8845", false, true,
				                                              {config_path.parent_path()}),
				             label +
				                 ": update_config_value accepts yunet_score_threshold dot decimal");
				ok &= Expect(
				    ReadConfigTestFile(config_path).contains("yunet_score_threshold = 0.8845\n"),
				    label + ": yunet_score_threshold written unchanged");
				ok &=
				    Expect(howdy::native::UpdateConfigValue(config_path, "yunet_nms_threshold",
					                                        nullptr, "0.3", false, true,
					                                        {config_path.parent_path()}),
					       label + ": update_config_value accepts yunet_nms_threshold dot decimal");
				ok &=
				    Expect(ReadConfigTestFile(config_path).contains("yunet_nms_threshold = 0.3\n"),
					       label + ": yunet_nms_threshold written unchanged");
				ok &= Expect(howdy::native::UpdateConfigValue(config_path, "sface_threshold",
				                                              nullptr, "0.6942", false, true,
				                                              {config_path.parent_path()}),
				             label + ": update_config_value accepts sface_threshold dot decimal");
				ok &= Expect(ReadConfigTestFile(config_path).contains("sface_threshold = 0.6942\n"),
				             label + ": sface_threshold written unchanged");

				for (const auto *const value : {"1,25", "1.25abc", "nan", "inf", "+inf", "-inf"}) {
					const auto before_invalid = ReadConfigTestFile(config_path);
					ok &= Expect(!howdy::native::UpdateConfigValue(config_path, "clahe_clip_limit",
					                                               nullptr, value, false, true,
					                                               {config_path.parent_path()}),
					             label + ": update_config_value rejects invalid float " +
					                 std::string(value));
					ok &= Expect(ReadConfigTestFile(config_path) == before_invalid,
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
		    "config.toml", howdy::native::DefaultSecureOwnerUid(), {context.temp_root});
		ok &= Expect(!parentless_check.ok &&
		                 parentless_check.error_message.contains("must have a parent directory"),
		             "check_secure_config_path rejects parentless config path");

		ok &= Expect(WriteConfigTestFile(context.config_path, context.config_at_limit),
		             "write config exactly at size limit");
		std::string size_error;
		// The comment-only fixture has no "disabled" key, so the update must get past the read
		// and fail only because appending the option exceeds the size limit.
		const bool at_limit_updated =
		    howdy::native::UpdateConfigValue(context.config_path, "disabled", &size_error, "true",
			                                 false, false, {context.temp_root});
		ok &= Expect(!at_limit_updated && size_error.contains("exceeds maximum size"),
		             "config exactly at size limit is read before the size limit fails: " +
		                 size_error);
		size_error.clear();
		ok &= Expect(WriteConfigTestFile(context.config_path, context.config_over_limit),
		             "write oversized config");
		ok &= Expect(!howdy::native::UpdateConfigValue(context.config_path, "disabled", &size_error,
		                                               "true", false, false, {context.temp_root}),
		             "update_config_value rejects oversized current config");
		ok &= Expect(size_error == "Failed to read config file",
		             "oversized update reports config read failure");
		ok &= Expect(ReadConfigTestFile(context.config_path) == context.config_over_limit,
		             "oversized update leaves current config unchanged");
		ok &= Expect(WriteConfigTestFile(context.config_path, "[core]\n"
		                                                      "disabled = false\n"
		                                                      "\n"
		                                                      "[video]\n"
		                                                      "dark_threshold = 60\n"),
		             "restore initial config after size-limit reads");

		ok &= Expect(howdy::native::UpdateConfigValue(context.config_path, "disabled", nullptr,
		                                              "true", false, true, {context.temp_root}),
		             "update_config_value succeeds for existing key");
		const auto after_disabled = ReadConfigTestFile(context.config_path);
		ok &= Expect(after_disabled.contains("disabled = true\n"), "disabled key updated");

		constexpr auto expected_scalar_error = "Config values must be printable ASCII";
		const auto     before_unsafe         = ReadConfigTestFile(context.config_path);

		for (const auto &unsafe_value : {
		         std::string(1, '\0'),
		         std::string("\r"),
		         std::string("true\n[video]\ntimeout = 0"),
		         std::string("/dev/video0\t"),
		         std::string("\x1b[2J"),
		     }) {
			std::string error;
			ok &= Expect(!howdy::native::UpdateConfigValue(context.config_path, "disabled", &error,
			                                               unsafe_value, false, false,
			                                               {context.temp_root}),
			             "update_config_value rejects unsafe scalar value");
			ok &= Expect(error == expected_scalar_error,
			             "update_config_value returns exact scalar diagnostic");
			ok &= Expect(ReadConfigTestFile(context.config_path) == before_unsafe,
			             "unsafe scalar update leaves config file unchanged");
		}

		for (const auto &[key, expected] : {
		         std::pair<std::string, std::string>{"no_such_option",
				                                     "Could not find a \"no_such_option\" "
				                                     "config option to set"},
		         std::pair<std::string, std::string>{"\x1b]0;x\a",
				                                     "Config keys must be printable ASCII"},
		     }) {
			std::string error;
			ok &= Expect(!howdy::native::UpdateConfigValue(context.config_path, key, &error, "1",
			                                               false, false, {context.temp_root}),
			             "update_config_value rejects unknown key");
			ok &= Expect(error == expected, "unknown key diagnostic: " + error);
			ok &= Expect(ReadConfigTestFile(context.config_path) == before_unsafe,
			             "unknown key update leaves config file unchanged");
		}

		std::string nonexistent_path_error;
		ok &= Expect(!howdy::native::UpdateConfigValue(
		                 context.temp_root / "nonexistent.toml", "disabled",
		                 &nonexistent_path_error, "line\nbreak", false, false, {context.temp_root}),
		             "unsafe scalar fails before filesystem/path checks");
		ok &= Expect(nonexistent_path_error == expected_scalar_error,
		             "unsafe scalar on nonexistent path returns scalar diagnostic");

		const auto empty_scalar_path = context.temp_root / "empty-scalar.toml";
		ok &= Expect(WriteConfigTestFile(empty_scalar_path, "[video]\ndevice_path = \"none\"\n"),
		             "write config for empty scalar update");
		ok &= Expect(howdy::native::UpdateConfigValue(empty_scalar_path, "device_path", nullptr, "",
		                                              false, false, {context.temp_root}),
		             "update_config_value accepts empty scalar value");
		ok &= Expect(ReadConfigTestFile(empty_scalar_path) == "[video]\ndevice_path = \"\"\n",
		             "empty scalar value is written as an empty TOML string");

		const auto reset_path = context.temp_root / "reset.toml";
		ok &=
		    Expect(WriteConfigTestFile(reset_path, "[video]\ntimeout = 9 # mine\ndark_threshold = "
			                                       "60.0\n"),
			       "write config for empty numeric update");
		ok &= Expect(howdy::native::UpdateConfigValue(reset_path, "timeout", nullptr, "", false,
		                                              true, {context.temp_root}),
		             "empty value for a number resets it to the default");
		ok &= Expect(ReadConfigTestFile(reset_path) ==
		                 "[video]\ntimeout = 4 # mine\ndark_threshold = 60.0\n",
		             "reset writes the schema default and keeps the key and its comment");
		ok &= Expect(howdy::native::UpdateConfigValue(reset_path, "timeout", nullptr, "", false,
		                                              true, {context.temp_root}) &&
		                 ReadConfigTestFile(reset_path) ==
		                     "[video]\ntimeout = 4 # mine\ndark_threshold = 60.0\n",
		             "resetting again leaves the file unchanged");

		const auto crlf_path = context.temp_root / "crlf.toml";
		ok &= Expect(WriteConfigTestFile(crlf_path, "[video]\r\ndark_threshold = 60.0\r\n"),
		             "write CRLF config");
		ok &= Expect(howdy::native::UpdateConfigValue(crlf_path, "timeout", nullptr, "5", false,
		                                              true, {context.temp_root}) &&
		                 howdy::native::UpdateConfigValue(crlf_path, "disabled", nullptr, "true",
		                                                  false, true, {context.temp_root}),
		             "insert key and table into CRLF config");
		ok &= Expect(ReadConfigTestFile(crlf_path) ==
		                 "[video]\ndark_threshold = 60.0\ntimeout = 5\n\n[core]\ndisabled = true\n",
		             "a CRLF config is rewritten with LF line endings: " +
		                 ReadConfigTestFile(crlf_path));

		const auto unicode_path = context.temp_root / "unicode.toml";
		ok &= Expect(WriteConfigTestFile(unicode_path,
		                                 "\xEF\xBB\xBF[video]\ndevice_path = "
		                                 "\"/dev/v4l/by-id/usb-Camera\" # cam\xC3\xA9ra avant\n"),
		             "write config with a non-ASCII comment and BOM");
		ok &= Expect(howdy::native::UpdateConfigValue(unicode_path, "device_path", nullptr,
		                                              "/dev/video2", false, true,
		                                              {context.temp_root}),
		             "update line with a non-ASCII comment");
		ok &= Expect(ReadConfigTestFile(unicode_path) ==
		                 "[video]\ndevice_path = \"/dev/video2\" # cam\xC3\xA9ra avant\n",
		             "non-ASCII comment survives the edit; the BOM is dropped");
		const auto bom_line_path = context.temp_root / "bom-line.toml";
		ok &= Expect(WriteConfigTestFile(bom_line_path, "\xEF\xBB\xBF"
		                                                "[core]\ndisabled = false # é\n"),
		             "write config with BOM");
		ok &= Expect(howdy::native::UpdateConfigValue(bom_line_path, "disabled", nullptr, "true",
		                                              false, true, {context.temp_root}) &&
		                 ReadConfigTestFile(bom_line_path) == "[core]\ndisabled = true # é\n",
		             "edit after a BOM keeps the comment");

		ok &=
		    Expect(howdy::native::UpdateConfigValue(context.config_path, "dark_threshold", nullptr,
			                                        "42", false, true, {context.temp_root}),
			       "update_config_value succeeds in later section");
		const auto after_threshold = ReadConfigTestFile(context.config_path);
		ok &= Expect(after_threshold.contains("dark_threshold = 42.0\n"),
		             "float option is written as a TOML float");

		const auto whitespace_path = context.temp_root / "whitespace.toml";
		ok &= Expect(WriteConfigTestFile(whitespace_path, "   \n\t  "),
		             "write whitespace-only config lines");
		ok &= Expect(!howdy::native::UpdateConfigValue(whitespace_path, "missing_key", nullptr, "x",
		                                               false, false, {context.temp_root}),
		             "update_config_value rejects unknown option names");

		// NOLINTBEGIN(bugprone-easily-swappable-parameters)
		const auto edit_case = [&](const std::string &name, const std::string &before,
		                           const std::string &key, const std::string &value,
		                           const std::string &expected) -> bool {
			const auto  path = context.temp_root / (name + ".toml");
			std::string error;
			bool        case_ok = Expect(WriteConfigTestFile(path, before), name + ": write");
			case_ok &= Expect(howdy::native::UpdateConfigValue(path, key, &error, value, false,
			                                                   true, {context.temp_root}),
			                  name + ": update succeeds: " + error);
			const auto after = ReadConfigTestFile(path);
			case_ok &=
			    Expect(after == expected, name + ": expected:\n" + expected + "got:\n" + after);
			return case_ok;
		};
		const auto refused_case = [&](const std::string &name, const std::string &before,
		                              const std::string &key, const std::string &value,
		                              const std::string &error_fragment) -> bool {
			const auto  path = context.temp_root / (name + ".toml");
			std::string error;
			bool        case_ok = Expect(WriteConfigTestFile(path, before), name + ": write");
			case_ok &= Expect(!howdy::native::UpdateConfigValue(path, key, &error, value, false,
			                                                    true, {context.temp_root}),
			                  name + ": update is refused");
			case_ok &= Expect(error.contains(error_fragment),
			                  name + ": error mentions " + error_fragment + ", got: " + error);
			case_ok &= Expect(ReadConfigTestFile(path) == before, name + ": file unchanged");
			return case_ok;
		};
		// NOLINTEND(bugprone-easily-swappable-parameters)

		ok &= edit_case(
		    "preserve-comments",
		    "# header\n[core]\n# note\n  disabled=false   # trailing\nabort_if_ssh = true\n",
		    "disabled", "true",
		    "# header\n[core]\n# note\n  disabled=true   # trailing\nabort_if_ssh = true\n");
		ok &= edit_case("bom", "\xEF\xBB\xBF[core]\ndisabled=false\n", "disabled", "true",
		                "[core]\ndisabled=true\n");
		ok &= edit_case("int-normalized", "[video]\ntimeout = 4\n", "timeout", "+5",
		                "[video]\ntimeout = 5\n");
		ok &= edit_case("string-quoted", "[video]\ndevice_path = \"none\"\n", "device_path",
		                "/dev/video2", "[video]\ndevice_path = \"/dev/video2\"\n");
		ok &= edit_case("insert-into-table",
		                "[core]\ndisabled = false\n# tail comment\n\n[video]\ntimeout = 4\n",
		                "abort_if_ssh", "false",
		                "[core]\ndisabled = false\nabort_if_ssh = false\n# tail comment\n\n"
		                "[video]\ntimeout = 4\n");
		ok &= edit_case("insert-after-header", "[core]\n\n[video]\ntimeout = 4", "disabled", "true",
		                "[core]\ndisabled = true\n\n[video]\ntimeout = 4\n");
		ok &= edit_case("insert-no-trailing-newline", "[video]\ntimeout = 4", "rotate", "2",
		                "[video]\ntimeout = 4\nrotate = 2\n");
		ok &= edit_case("append-table", "[core]\ndisabled = false", "end_report", "true",
		                "[core]\ndisabled = false\n\n[debug]\nend_report = true\n");
		ok &= edit_case("append-to-empty", "", "timeout", "9", "[video]\ntimeout = 9\n");
		ok &= edit_case(
		    "comment-mentions-key",
		    "[core]\n# disabled = false\nabort_if_ssh = true # disabled\ndisabled = false\n",
		    "disabled", "true",
		    "[core]\n# disabled = false\nabort_if_ssh = true # disabled\ndisabled = true\n");

		ok &= refused_case("invalid-toml", "[core]\ndisabled true\n", "disabled", "false",
		                   "not valid TOML");
		ok &= refused_case("duplicate-target", "[core]\ndisabled = true\ndisabled = false\n",
		                   "disabled", "true", "not valid TOML");
		ok &= edit_case("inline-table", "core = { disabled = false }\n", "disabled", "true",
		                "core = { disabled = true }\n");
		ok &= edit_case("dotted-key", "core.disabled = false # note\n", "disabled", "true",
		                "core.disabled = true # note\n");
		ok &= edit_case("multi-line-string", "[video]\ndevice_path = \"\"\"\n/dev/video0\"\"\"\n",
		                "device_path", "/dev/video1", "[video]\ndevice_path = \"/dev/video1\"\n");
		ok &= refused_case("mixed-case-table", "[CORE]\nDISABLED = false\n", "disabled", "true",
		                   "unknown table [CORE]");
		ok &=
		    refused_case("invalid-int-text", "[video]\ntimeout = 4\n", "timeout", "abc",
			             "Invalid config value for timeout=\"abc\": expected integer range 1..300");
		ok &= refused_case("invalid-bool-text", "[core]\ndisabled = false\n", "disabled", "maybe",
		                   "Invalid config value for disabled=\"maybe\": expected a boolean");
		ok &= refused_case("bool-alias-spelling", "[core]\ndisabled = false\n", "disabled", "yes",
		                   "Invalid config value for disabled=\"yes\": expected a boolean");
		ok &= refused_case("control-char-string", "[video]\ndevice_path = \"none\"\n",
		                   "device_path", std::string("/dev/video\x01"), "must be printable ASCII");

		const auto recovery_path = context.temp_root / "recovery-structure.toml";
		ok &= Expect(WriteConfigTestFile(recovery_path, "[CORE]\nDISABLED = false\n"),
		             "write structurally invalid config for recovery write");
		std::string recovery_error;
		ok &= Expect(!howdy::native::UpdateConfigValue(recovery_path, "disabled", &recovery_error,
		                                               "true", false, false, {context.temp_root}),
		             "recovery writes still require schema-valid TOML");
		ok &= Expect(recovery_error.contains("unknown table [CORE]"),
		             "recovery write reports structural error: " + recovery_error);

		ok &= Expect(!howdy::native::UpdateConfigValue(context.config_path, "missing_key", nullptr,
		                                               "x", false, true, {context.temp_root}),
		             "update_config_value fails for missing key");
		ok &= Expect(!howdy::native::UpdateConfigValue(context.config_path, "dark_threshold",
		                                               nullptr, "0\n[core]\ndisabled = true", false,
		                                               true, {context.temp_root}),
		             "update_config_value rejects newline injection");
		ok &= Expect(ReadConfigTestFile(context.config_path) == after_threshold,
		             "rejected injection leaves config unchanged");
		ok &=
		    Expect(!howdy::native::UpdateConfigValue(context.config_path, "dark_threshold", nullptr,
			                                         "1000", false, true, {context.temp_root}),
			       "update_config_value rejects semantically invalid values");
		ok &= Expect(ReadConfigTestFile(context.config_path) == after_threshold,
		             "semantic validation failure leaves config unchanged");
		ok &= Expect(!howdy::native::UpdateConfigValue(context.config_path, "device_fps", nullptr,
		                                               "-1", false, true, {context.temp_root}),
		             "update_config_value rejects invalid device_fps values");
		ok &= Expect(ReadConfigTestFile(context.config_path) == after_threshold,
		             "invalid device_fps update leaves config unchanged");

		ok &= ExpectFloatWritePaths(context.config_path);

		ok &= Expect(WriteConfigTestFile(context.config_path, "[core]\n"
		                                                      "disabled = false\n"
		                                                      "\n"
		                                                      "[video]\n"
		                                                      "timeout = 0\n"),
		             "write config with invalid timeout");
		ok &= Expect(howdy::native::UpdateConfigValue(context.config_path, "disabled", nullptr,
		                                              "true", false, false, {context.temp_root}),
		             "update_config_value can bypass runtime validation for recovery writes");
		const auto after_disable_recovery = ReadConfigTestFile(context.config_path);
		ok &= Expect(after_disable_recovery.contains("disabled = true\n"),
		             "recovery write updates disabled key");
		const auto still_invalid =
		    howdy::native::test_support::ReadConfigDocument(context.config_path);
		ok &= Expect(still_invalid.Ok(), "recovery-write config still parses");
		ok &= Expect(howdy::native::ValidateRuntimeConfig(still_invalid).has_value(),
		             "recovery write does not mask unrelated invalid config values");
		ok &= Expect(WriteConfigTestFile(context.config_path, after_threshold),
		             "restore valid config after recovery-write test");

		return ok;
	}

}  // namespace howdy::test
