#include "../../src/config/config_document/internal.hpp"
#include "../../src/config/config_validation/internal.hpp"
#include "../../src/config/runtime_config_loader/internal.hpp"
#include "config/config_limits.hpp"
#include "config/config_schema.hpp"
#include "support/capture_device_path.hpp"
#include "support/face_metric.hpp"
#include "test_support.hpp"

#include <clocale>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

	using howdy::native::ConfigDocument;
	using howdy::native::SetConfigValue;
	using howdy::native::config_schema::ConfigValue;
	using howdy::native::config_schema::OptionId;
	using howdy::native::config_schema::RuntimeConfigOption;
	using howdy::test::Expect;
	using howdy::test::ScopedEnvironmentVariable;

	auto Near(float value, float expected) -> bool {
		constexpr float tolerance = 0.0001F;
		return value > expected - tolerance && value < expected + tolerance;
	}

	auto Parse(std::string_view content) -> ConfigDocument {
		return ConfigDocument::FromContent(content);
	}

	struct Rejection {
		std::string_view content;
		std::string_view error_fragment;
	};

	auto ExpectRejected(Rejection rejection, const std::string &label) -> bool {
		const auto document       = Parse(rejection.content);
		const auto error_fragment = rejection.error_fragment;
		bool       ok             = Expect(!document.Ok(), label + ": rejected");
		ok &= Expect(document.Error().contains(error_fragment), label + ": error mentions \"" +
		                                                            std::string(error_fragment) +
		                                                            "\", got: " + document.Error());
		return ok;
	}

	auto TestTypedParsing() -> bool {
		bool ok = true;

		const auto valid = Parse("# comment\n"
		                         "[core]\n"
		                         "disabled = true # trailing comment\n"
		                         "[video]\n"
		                         "timeout = 7\n"
		                         "dark_threshold = 55.5\n"
		                         "max_height = 480\n"
		                         "device_path = \"/dev/video0\"\n");
		ok &= Expect(valid.Ok(), "valid config parses: " + valid.Error());
		ok &= Expect(!howdy::native::ValidateRuntimeConfig(valid).has_value(),
		             "valid config passes semantic validation");
		ok &= Expect(valid.Value("core", "disabled") != nullptr &&
		                 *valid.Value("core", "disabled") == ConfigValue{true},
		             "bool is stored with its schema type");
		ok &= Expect(valid.GetBool("core", "disabled", false), "bool value");
		ok &= Expect(valid.GetInt("video", "timeout", 3) == 7, "integer value");
		ok &= Expect(Near(valid.GetFloat("video", "dark_threshold", 1.0F), 55.5F), "float value");
		ok &= Expect(valid.GetFloat("video", "max_height", 1.0F) == 480.0F,
		             "integer literal accepted for float option");
		ok &= Expect(valid.GetString("video", "device_path", "") == "/dev/video0", "string value");
		ok &= Expect(valid.GetString("core", "missing", "fallback") == "fallback",
		             "missing fallback");
		ok &= Expect(valid.Value("core", "abort_if_ssh") == nullptr, "omitted option is unset");
		ok &= Expect(howdy::native::ReadRuntimeInt(valid, OptionId::kVideoTimeout) == 7,
		             "runtime int reader keeps configured value");

		const auto empty = Parse("");
		ok &= Expect(empty.Ok(), "empty config parses");
		ok &= Expect(howdy::native::ReadRuntimeInt(empty, OptionId::kVideoTimeout) == 4,
		             "timeout default is used");
		ok &=
		    Expect(Near(howdy::native::ReadRuntimeFloat(empty, OptionId::kVideoMaxHeight), 320.0F),
			       "max height default is used");
		ok &= Expect(
		    howdy::native::ReadRuntimeString(empty, OptionId::kFaceSfaceMetric) ==
		        howdy::native::config_schema::RuntimeDefaultString(OptionId::kFaceSfaceMetric),
		    "sface metric default is used");
		ok &= Expect(
		    Near(howdy::native::ReadSfaceThreshold(empty, howdy::native::FaceMetric::kCosine),
			     0.6942F),
		    "cosine sface threshold default is used");

		const auto empty_string = Parse("[video]\ndevice_path = \"\"\n");
		ok &= Expect(empty_string.Ok() && empty_string.Value("video", "device_path") == nullptr,
		             "empty string counts as unset");
		ok &= Expect(howdy::native::ReadRuntimeString(empty_string, OptionId::kVideoDevicePath) ==
		                 howdy::native::kNoCaptureDevice,
		             "empty string selects default");

		const auto inline_table = Parse("core = { disabled = true }\n");
		ok &= Expect(inline_table.Ok() && inline_table.GetBool("core", "disabled", false),
		             "inline table is valid TOML for runtime loading");
		const auto dotted = Parse("video.timeout = 9\n");
		ok &= Expect(dotted.Ok() && dotted.GetInt("video", "timeout", 0) == 9,
		             "dotted key is valid TOML for runtime loading");

		const auto boundaries = Parse("[video]\n"
		                              "timeout = 1\n"
		                              "frame_width = -1\n"
		                              "frame_height = 8192\n"
		                              "exposure = -1\n"
		                              "device_fps = 480\n"
		                              "dark_threshold = 0\n"
		                              "max_height = 4096.0\n");
		ok &= Expect(boundaries.Ok() && !howdy::native::ValidateRuntimeConfig(boundaries),
		             "boundary values validate");
		ok &= Expect(howdy::native::ReadRuntimeInt(boundaries, OptionId::kVideoFrameWidth) == -1 &&
		                 howdy::native::ReadRuntimeInt(boundaries, OptionId::kVideoFrameHeight) ==
		                     8192,
		             "sentinel and maximum integers are kept");
		return ok;
	}

	auto TestRejections() -> bool {
		bool ok = true;
		ok &= ExpectRejected(
		    {
		        .content        = "[core\n",
		        .error_fragment = "line 1",
		    },
		    "syntax error reports position");
		ok &= ExpectRejected(
		    {
		        .content        = "[core]\ndisabled = yes\n",
		        .error_fragment = "line 2",
		    },
		    "bare legacy boolean");
		ok &= ExpectRejected(
		    {
		        .content        = "[core]\ndisabled = \"true\"\n",
		        .error_fragment = "must be a boolean",
		    },
		    "quoted boolean");
		ok &= ExpectRejected(
		    {
		        .content        = "[core]\ndisabled = 1\n",
		        .error_fragment = "must be a boolean",
		    },
		    "integer boolean");
		ok &= ExpectRejected(
		    {
		        .content        = "[video]\ntimeout = \"4\"\n",
		        .error_fragment = "must be an integer",
		    },
		    "quoted int");
		ok &= ExpectRejected(
		    {
		        .content        = "[video]\ntimeout = 4.0\n",
		        .error_fragment = "must be an integer",
		    },
		    "float for int");
		ok &= ExpectRejected(
		    {
		        .content        = "[video]\ntimeout = 4294967296\n",
		        .error_fragment = "out of range",
		    },
		    "int64 overflow");
		ok &= ExpectRejected(
		    {
		        .content        = "[video]\nmax_height = \"320\"\n",
		        .error_fragment = "must be a number",
		    },
		    "quoted float");
		ok &= ExpectRejected(
		    {
		        .content        = "[video]\ndark_threshold = nan\n",
		        .error_fragment = "must be finite",
		    },
		    "nan");
		ok &= ExpectRejected(
		    {
		        .content        = "[video]\ndark_threshold = inf\n",
		        .error_fragment = "must be finite",
		    },
		    "inf");
		ok &= ExpectRejected(
		    {
		        .content        = "[video]\ndark_threshold = 1e300\n",
		        .error_fragment = "must be finite",
		    },
		    "float overflow");
		ok &= ExpectRejected(
		    {
		        .content        = "[video]\ndevice_path = 0\n",
		        .error_fragment = "must be a string",
		    },
		    "int for string");
		ok &= ExpectRejected(
		    {
		        .content        = "[video]\ndevice_path = [\"/dev/video0\"]\n",
		        .error_fragment = "must be a string",
		    },
		    "array value");
		ok &= ExpectRejected(
		    {
		        .content        = "[core]\ndisabeld = true\n",
		        .error_fragment = "unknown key \"core.disabeld\"",
		    },
		    "unknown key typo");
		ok &= ExpectRejected(
		    {
		        .content        = "[extra]\nvalue = 1\n",
		        .error_fragment = "unknown table [extra]",
		    },
		    "unknown table");
		ok &= ExpectRejected(
		    {
		        .content        = "timeout = 4\n",
		        .error_fragment = "\"timeout\" must be a table",
		    },
		    "top-level value");
		ok &= ExpectRejected(
		    {
		        .content        = "[[core]]\ndisabled = true\n",
		        .error_fragment = "\"core\" must be a table",
		    },
		    "array of tables");
		for (const auto *const escaped : {
		         "\\u0000x",
		         "\\n",
		         "\\r",
		         "\\t",
		         "\\u001b[2J",
		         "\\u007f",
		         "\\u009b",
		         "\\u202e",
		         "\\u2028",
		         "\\u200b",
		     }) {
			ok &= ExpectRejected(
			    {
			        .content =
			            std::string("[video]\ndevice_path = \"/dev/video0") + escaped + "\"\n",
			        .error_fragment = "device_path must be printable ASCII",
			    },
			    std::string("escaped control character ") + escaped);
		}
		ok &= ExpectRejected(
		    {
		        .content        = "[core.nested]\nvalue = 1\n",
		        .error_fragment = "core.nested must be a string, integer, float, or boolean",
		    },
		    "nested table");
		ok &= ExpectRejected(
		    {
		        .content        = "[core]\ndisabled = true\ndisabled = false\n",
		        .error_fragment = "duplicate key",
		    },
		    "duplicate key");
		ok &= ExpectRejected(
		    {
		        .content        = "[Core]\ndisabled = true\n",
		        .error_fragment = "unknown table [Core]",
		    },
		    "keys are case-sensitive");
		ok &= ExpectRejected(
		    {
		        .content        = "[video]\ntimeout = 1979-05-27\n",
		        .error_fragment = "video.timeout must be a string, integer, float, or boolean",
		    },
		    "datetime value");
		return ok;
	}

	auto TestSemanticValidation() -> bool {
		bool ok = true;

		const auto invalid = Parse("[face]\n"
		                           "sface_metric = \"weird\"\n"
		                           "sface_threshold = 9\n"
		                           "yunet_top_k = 0\n"
		                           "[video]\n"
		                           "timeout = 0\n"
		                           "dark_threshold = 1000\n"
		                           "rotate = 4\n"
		                           "frame_width = 4\n");
		ok &= Expect(invalid.Ok(), "out-of-range values are structurally valid");
		ok &= Expect(howdy::native::ValidateRuntimeConfig(invalid).has_value(),
		             "out-of-range values fail semantic validation");
		ok &= Expect(howdy::native::ReadRuntimeInt(invalid, OptionId::kVideoTimeout) == 4,
		             "invalid timeout falls back");
		ok &= Expect(howdy::native::ReadRuntimeInt(invalid, OptionId::kVideoFrameWidth) == -1,
		             "invalid frame width falls back");
		ok &= Expect(!howdy::native::ReadSfaceMetric(invalid).has_value(),
		             "typed metric reader rejects unknown choice");

		const auto negative_fps = Parse("[video]\ndevice_fps = -1\n");
		const auto validation   = howdy::native::ValidateRuntimeConfig(negative_fps);
		ok &= Expect(validation.has_value() && validation->contains("device_fps") &&
		                 validation->contains("-1"),
		             "validation message reports key and value");

		const auto uppercase = Parse("[face]\nsface_metric = \"L2NORM\"\n");
		ok &= Expect(howdy::native::ValidateRuntimeConfig(uppercase).has_value(),
		             "choice strings are case-sensitive");

		const auto wide = Parse("[face]\nsface_threshold = 3\n");
		ok &= Expect(
		    Near(howdy::native::ReadSfaceThreshold(wide, howdy::native::FaceMetric::kL2), 3.0F),
		    "l2 keeps threshold of 3");
		ok &=
		    Expect(Near(howdy::native::ReadSfaceThreshold(wide, howdy::native::FaceMetric::kL2Norm),
			            0.6942F),
			       "l2norm falls back from a threshold above 2");

		const auto &metric_option =
		    howdy::native::config_schema::RuntimeConfigOption(OptionId::kFaceSfaceMetric);
		for (const auto choice : metric_option.choices) {
			const auto document = Parse("[face]\nsface_metric = \"" + std::string(choice) + "\"\n");
			const auto parsed_metric = howdy::native::ReadSfaceMetric(document);
			ok &= Expect(parsed_metric.has_value() &&
			                 howdy::native::FaceMetricSpelling(*parsed_metric) == choice,
			             "typed metric reader accepts schema choice " + std::string(choice));
		}
		return ok;
	}

	auto TestFloatLocale() -> bool {
		bool       ok       = true;
		const auto validate = [&](const std::string &label) -> bool {
			const auto document = Parse("[video]\nclahe_clip_limit = 1.25\n"
			                            "[face]\nyunet_score_threshold = 0.8845\n");
			return Expect(
			    document.Ok() && !howdy::native::ValidateRuntimeConfig(document) &&
			        Near(document.GetFloat("video", "clahe_clip_limit", 0.0F), 1.25F) &&
			        Near(document.GetFloat("face", "yunet_score_threshold", 0.0F), 0.8845F),
			    label + ": dot-decimal floats parse independent of locale");
		};
		ok &= validate("C locale");

		const char *current_locale = std::setlocale(LC_ALL, nullptr);
		const auto  previous_locale =
		    current_locale == nullptr ? std::optional<std::string>() : std::string(current_locale);
		{
			const ScopedEnvironmentVariable lc_all("LC_ALL");
			const ScopedEnvironmentVariable lang("LANG", "C");
			const ScopedEnvironmentVariable lc_numeric("LC_NUMERIC", "nl_NL.UTF-8");
			if (std::setlocale(LC_ALL, "") != nullptr) {
				ok &= validate("LC_NUMERIC=nl_NL.UTF-8");
			} else {
				std::cerr << "SKIP: nl_NL.UTF-8 locale is not generated\n";
			}
		}
		if (previous_locale.has_value()) {
			ok &= Expect(std::setlocale(LC_ALL, previous_locale->c_str()) != nullptr,
			             "restore original locale");
		}

		const auto &clip = RuntimeConfigOption(OptionId::kVideoClaheClipLimit);
		ok &= Expect(howdy::native::ParseOptionValueText(clip, "1.25") == ConfigValue{1.25F},
		             "set input float parses independent of locale");
		return ok;
	}

	auto TestSetConfigValue() -> bool {
		const auto &disabled = RuntimeConfigOption(OptionId::kCoreDisabled);
		const auto &timeout  = RuntimeConfigOption(OptionId::kVideoTimeout);
		const auto &height   = RuntimeConfigOption(OptionId::kVideoMaxHeight);
		const auto &device   = RuntimeConfigOption(OptionId::kVideoDevicePath);

		const auto replaced = SetConfigValue("[core]\n# keep\ndisabled = false  # note\n", disabled,
		                                     ConfigValue{true});
		bool       ok =
		    Expect(replaced.ok && replaced.content == "[core]\n# keep\ndisabled = true  # note\n",
			       "replaces value and keeps comments: " + replaced.content);

		const auto inserted = SetConfigValue("[core]\ndisabled = false\n", timeout, ConfigValue{9});
		const auto inserted_document = ConfigDocument::FromContent(inserted.content);
		ok &= Expect(inserted.ok && inserted_document.GetInt("video", "timeout", 0) == 9,
		             "appends missing table: " + inserted.content);

		const auto floating = SetConfigValue("[video]\n", height, ConfigValue{0.3F});
		ok &= Expect(floating.ok && floating.content.contains("max_height = 0.3\n"),
		             "writes shortest float text: " + floating.content);

		const auto quoted = SetConfigValue("[video]\n", device, ConfigValue{std::string("/a\"b")});
		ok &= Expect(
		    quoted.ok &&
		        ConfigDocument::FromContent(quoted.content).GetString("video", "device_path", "") ==
		            "/a\"b",
		    "escapes strings: " + quoted.content);

		const auto invalid = SetConfigValue("[core\n", disabled, ConfigValue{true});
		ok &= Expect(!invalid.ok && invalid.error.contains("line 1"),
		             "refuses invalid TOML with position: " + invalid.error);
		return ok;
	}

	auto TestParseOptionValueText() -> bool {
		using howdy::native::ParseOptionValueText;
		const auto &disabled = RuntimeConfigOption(OptionId::kCoreDisabled);
		const auto &timeout  = RuntimeConfigOption(OptionId::kVideoTimeout);
		const auto &clip     = RuntimeConfigOption(OptionId::kVideoClaheClipLimit);
		const auto &metric   = RuntimeConfigOption(OptionId::kFaceSfaceMetric);
		const auto &device   = RuntimeConfigOption(OptionId::kVideoDevicePath);

		bool ok = Expect(ParseOptionValueText(disabled, "true") == ConfigValue{true},
		                 "TOML boolean literal is accepted");
		ok &=
		    Expect(!ParseOptionValueText(disabled, "yes").has_value(), "boolean alias is rejected");
		ok &= Expect(!ParseOptionValueText(disabled, "maybe").has_value(), "invalid boolean");
		ok &= Expect(ParseOptionValueText(metric, "COSINE") == ConfigValue{std::string("COSINE")},
		             "choice string is kept as given for validation");
		ok &= Expect(ParseOptionValueText(device, "/dev/Video0") ==
		                 ConfigValue{std::string("/dev/Video0")},
		             "device path keeps case");
		ok &= Expect(!ParseOptionValueText(device, "/dev/v\n").has_value(),
		             "control characters rejected");
		// U+202E (right-to-left override) built from bytes so the source holds no bidi literal.
		const std::string right_to_left_override = {'\xE2', '\x80', '\xAE'};
		ok &= Expect(!ParseOptionValueText(device, "/dev/" + right_to_left_override).has_value(),
		             "bidi formatting characters rejected");
		ok &= Expect(ParseOptionValueText(timeout, "+5") == ConfigValue{5} &&
		                 ParseOptionValueText(timeout, "1_000") == ConfigValue{1000} &&
		                 ParseOptionValueText(timeout, "0x10") == ConfigValue{16},
		             "integers use TOML literal syntax");
		ok &= Expect(!ParseOptionValueText(clip, "1e-50").has_value() &&
		                 !ParseOptionValueText(clip, "1e-40").has_value() &&
		                 ParseOptionValueText(clip, "0.0") == ConfigValue{0.0F},
		             "floats that underflow are rejected; zero is kept");
		ok &= Expect(!ParseOptionValueText(device, "/dev/v\xC3\xA9").has_value(),
		             "non-ASCII string is rejected");
		ok &= Expect(ParseOptionValueText(clip, "2") == ConfigValue{2.0F} &&
		                 ParseOptionValueText(clip, "+1.5") == ConfigValue{1.5F},
		             "float options accept TOML integers and floats");
		for (const auto *const invalid :
		     {"+-1", "1.", ".5", "1,25", " 5", "5 ", "nan", "inf", "9999999999", "\"5\"", ""}) {
			ok &= Expect(!ParseOptionValueText(timeout, invalid).has_value(),
			             std::string("invalid integer text is rejected: ") + invalid);
		}
		for (const auto *const invalid : {"1.", ".5", "1,25", "nan", "inf", "1e300", "true"}) {
			ok &= Expect(!ParseOptionValueText(clip, invalid).has_value(),
			             std::string("invalid float text is rejected: ") + invalid);
		}
		for (const auto *const invalid : {"1", "0", "yes", "no", "on", "off", "True", "FALSE"}) {
			ok &= Expect(!ParseOptionValueText(disabled, invalid).has_value(),
			             std::string("boolean alias is rejected: ") + invalid);
		}
		return ok;
	}

	auto TestLimitsAndEncoding() -> bool {
		bool ok = ExpectRejected(
		    {
		        .content        = "[core]\ndisabled = \"\xff\"\n",
		        .error_fragment = "not valid UTF-8",
		    },
		    "invalid UTF-8");
		ok &= ExpectRejected(
		    {
		        .content        = "[core]\n\"\\u001b[2J\" = 1\n",
		        .error_fragment = "names must be printable ASCII",
		    },
		    "control character in key name");
		ok &= Expect(!Parse("[core]\n\"\\u001b[2J\" = 1\n").Error().contains('\x1b'),
		             "key name with control character is not echoed");
		const std::string oversized(howdy::native::kMaxConfigFileSize + 1, ' ');
		ok &= ExpectRejected({.content = oversized, .error_fragment = "maximum size"},
		                     "oversized config");
		return ok;
	}

}  // namespace

auto main() -> int {
	try {
		bool ok = true;

		ok &= TestTypedParsing();
		ok &= TestRejections();
		ok &= TestSemanticValidation();
		ok &= TestFloatLocale();
		ok &= TestSetConfigValue();
		ok &= TestParseOptionValueText();
		ok &= TestLimitsAndEncoding();

		ok &= Expect(howdy::native::IsAllowedCaptureDevicePath("/dev/v4l/by-id/usb-camera"),
		             "by-id device path prefix is allowed");
		ok &= Expect(!howdy::native::IsAllowedCaptureDevicePath("/tmp/camera"),
		             "non-device path prefix is rejected");

		return ok ? 0 : 1;
	} catch (const std::exception &error) {
		std::cerr << "FAIL: unexpected exception: " << error.what() << '\n';
	} catch (...) {
		std::cerr << "FAIL: unexpected non-standard exception\n";
	}
	return 1;
}
