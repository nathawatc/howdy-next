#include "config/config_reader.hpp"
#include "config/config_schema.hpp"
#include "config/config_validation.hpp"
#include "config/config_values.hpp"
#include "config/number_parsing.hpp"
#include "test_support.hpp"
#include "vision/capture_device_path.hpp"

#include <clocale>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

	using howdy::test::expect;
	using howdy::test::write_file;

	auto Near(float value, float expected) -> bool {
		constexpr float tolerance = 0.0001F;
		return value > expected - tolerance && value < expected + tolerance;
	}

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

}  // namespace

auto main() -> int {
	namespace fs = std::filesystem;
	bool ok      = true;
	using howdy::native::config_schema::OptionId;

	const auto      temp_root = fs::temp_directory_path() / "howdy-config-reader-test";
	std::error_code ec;
	fs::remove_all(temp_root, ec);
	fs::create_directories(temp_root, ec);
	ok &= expect(!ec, "create temp root");

	const auto valid_path = temp_root / "valid.ini";
	ok &= expect(write_file(valid_path, "[core]\n"
	                                    "disabled = true\n"
	                                    "[video]\n"
	                                    "timeout = 7\n"
	                                    "dark_threshold = 55.5\n"),
	             "write valid ini");

	howdy::native::ConfigReader valid(valid_path.string());
	ok &= expect(valid.Ok(), "valid config should parse");
	ok &= expect(!howdy::native::ValidateRuntimeConfig(valid).has_value(),
	             "valid config passes semantic validation");
	ok &= expect(valid.ParseError() == 0, "parse_error is zero for valid config");
	ok &= expect(valid.Get("core", "disabled", "false") == "true", "get string from valid config");
	ok &= expect(valid.Get("core", "missing", "fallback") == "fallback",
	             "fallback for missing string");
	ok &= expect(valid.GetInt("video", "timeout", 3) == 7, "get_int returns configured value");
	ok &= expect(valid.GetInt("video", "missing_timeout", 3) == 3,
	             "get_int fallback for missing value");
	ok &= expect(valid.GetFloat("video", "dark_threshold", 1.0F) > 55.4F &&
	                 valid.GetFloat("video", "dark_threshold", 1.0F) < 55.6F,
	             "get_float returns configured value");
	ok &= expect(valid.GetBool("core", "disabled", false), "get_bool returns configured true");
	ok &=
	    expect(valid.GetBool("core", "missing_bool", true), "get_bool fallback for missing value");
	ok &= expect(howdy::native::ReadRuntimeInt(valid, OptionId::kVideoTimeout) == 7,
	             "validated timeout keeps configured value");
	ok &= expect(howdy::native::ReadRuntimeFloat(valid, OptionId::kVideoDarkThreshold) > 55.4F &&
	                 howdy::native::ReadRuntimeFloat(valid, OptionId::kVideoDarkThreshold) < 55.6F,
	             "validated dark threshold keeps configured value");

	const auto empty_path = temp_root / "empty.ini";
	ok &= expect(write_file(empty_path, ""), "write empty ini");
	howdy::native::ConfigReader empty(empty_path.string());
	ok &= expect(empty.Ok(), "empty config should parse");
	ok &= expect(howdy::native::ReadRuntimeInt(empty, OptionId::kVideoTimeout) == 4,
	             "timeout default is used");
	ok &= expect(Near(howdy::native::ReadRuntimeFloat(empty, OptionId::kVideoDarkThreshold), 75.0F),
	             "dark threshold default is used");
	ok &= expect(Near(howdy::native::ReadRuntimeFloat(empty, OptionId::kVideoMaxHeight), 320.0F),
	             "max height default is used");
	ok &= expect(howdy::native::ReadRuntimeInt(empty, OptionId::kVideoRotate) == 0,
	             "rotate default is used");
	ok &= expect(howdy::native::ReadRuntimeInt(empty, OptionId::kVideoExposure) == -1,
	             "exposure default is used");
	ok &=
	    expect(Near(howdy::native::ReadRuntimeFloat(empty, OptionId::kVideoClaheClipLimit), 1.25F),
	           "clahe clip limit default is used");
	ok &= expect(howdy::native::ReadRuntimeInt(empty, OptionId::kVideoClaheTileGridSize) == 8,
	             "clahe tile grid size default is used");
	ok &= expect(howdy::native::ReadRuntimeInt(empty, OptionId::kVideoFrameWidth) == -1,
	             "frame width default is used");
	ok &= expect(howdy::native::ReadRuntimeInt(empty, OptionId::kVideoFrameHeight) == -1,
	             "frame height default is used");
	ok &= expect(howdy::native::ReadRuntimeInt(empty, OptionId::kVideoDeviceFps) == 0,
	             "device fps default is used");
	ok &= expect(
	    Near(howdy::native::ReadRuntimeFloat(empty, OptionId::kFaceYunetScoreThreshold), 0.8845F),
	    "yunet score threshold default is used");
	ok &=
	    expect(Near(howdy::native::ReadRuntimeFloat(empty, OptionId::kFaceYunetNmsThreshold), 0.3F),
	           "yunet nms threshold default is used");
	ok &= expect(howdy::native::ReadRuntimeInt(empty, OptionId::kFaceYunetTopK) == 1000,
	             "yunet top k default is used");
	ok &= expect(howdy::native::ReadRuntimeString(empty, OptionId::kFaceSfaceMetric) ==
	                 howdy::native::config_schema::RuntimeDefaultString(OptionId::kFaceSfaceMetric),
	             "sface metric default is used");
	const auto empty_metric = howdy::native::ReadSfaceMetric(empty);
	ok &= expect(empty_metric.has_value() &&
	                 *empty_metric == howdy::native::config_schema::kSfaceDefaultMetric,
	             "sface metric boundary parses default");
	ok &= expect(
	    Near(howdy::native::ReadSfaceThreshold(empty, howdy::native::FaceMetric::kCosine), 0.6942F),
	    "cosine sface threshold default is used");
	ok &= expect(
	    Near(howdy::native::ReadSfaceThreshold(empty, howdy::native::FaceMetric::kL2), 0.6942F),
	    "l2 sface threshold default is used");

	const auto bool_path = temp_root / "typed-bools.ini";
	ok &= expect(write_file(bool_path, "[core]\n"
	                                   "detection_notice = true\n"
	                                   "no_confirmation = false\n"
	                                   "abort_if_ssh = invalid\n"),
	             "write typed boolean config");
	howdy::native::ConfigReader typed_bools(bool_path.string());
	ok &= expect(howdy::native::ReadRuntimeBool(typed_bools, OptionId::kCoreDetectionNotice),
	             "generic bool reader accepts true");
	ok &= expect(!howdy::native::ReadRuntimeBool(typed_bools, OptionId::kCoreNoConfirmation),
	             "generic bool reader accepts false");
	ok &= expect(howdy::native::ReadRuntimeBool(typed_bools, OptionId::kCoreAbortIfSsh),
	             "generic bool reader uses schema fallback for invalid value");

	const auto integer_boundary_path = temp_root / "integer-boundaries.ini";
	ok &= expect(write_file(integer_boundary_path, "[video]\n"
	                                               "timeout = 1\n"
	                                               "frame_width = -1\n"
	                                               "frame_height = 8192\n"
	                                               "exposure = -1\n"
	                                               "device_fps = 480\n"),
	             "write integer boundary config");
	howdy::native::ConfigReader integer_boundaries(integer_boundary_path.string());
	ok &= expect(howdy::native::ReadRuntimeInt(integer_boundaries, OptionId::kVideoTimeout) == 1,
	             "generic int reader accepts minimum");
	ok &= expect(howdy::native::ReadRuntimeInt(integer_boundaries, OptionId::kVideoFrameHeight) ==
	                 8192,
	             "generic int reader accepts maximum");
	ok &=
	    expect(howdy::native::ReadRuntimeInt(integer_boundaries, OptionId::kVideoFrameWidth) == -1,
	           "generic int reader accepts allowed sentinel");
	ok &= expect(howdy::native::ReadRuntimeInt(integer_boundaries, OptionId::kVideoExposure) == -1,
	             "generic int reader accepts exposure sentinel");
	ok &=
	    expect(howdy::native::ReadRuntimeInt(integer_boundaries, OptionId::kVideoDeviceFps) == 480,
	           "generic int reader accepts fps maximum");

	const auto float_boundary_path = temp_root / "float-boundaries.ini";
	ok &= expect(write_file(float_boundary_path, "[video]\n"
	                                             "dark_threshold = 0\n"
	                                             "max_height = 4096\n"),
	             "write float boundary config");
	howdy::native::ConfigReader float_boundaries(float_boundary_path.string());
	ok &= expect(
	    Near(howdy::native::ReadRuntimeFloat(float_boundaries, OptionId::kVideoDarkThreshold),
	         0.0F),
	    "generic float reader accepts minimum");
	ok &= expect(howdy::native::ReadRuntimeFloat(float_boundaries, OptionId::kVideoMaxHeight) ==
	                 4096.0F,
	             "generic float reader accepts maximum");

	const auto &metric_option =
	    howdy::native::config_schema::RuntimeConfigOption(OptionId::kFaceSfaceMetric);
	for (const auto choice : metric_option.choices) {
		const auto choice_path = temp_root / ("metric-" + std::string(choice) + ".ini");
		ok &=
		    expect(write_file(choice_path, "[face]\nsface_metric = " + std::string(choice) + "\n"),
		           "write schema metric choice");
		howdy::native::ConfigReader choice_reader(choice_path.string());
		ok &= expect(choice_reader.Ok(), "choice reader parses: " + std::string(choice));
		ok &= expect(howdy::native::ReadRuntimeString(choice_reader, OptionId::kFaceSfaceMetric) ==
		                 choice,
		             "generic string reader accepts schema choice");
		const auto parsed_metric = howdy::native::ReadSfaceMetric(choice_reader);
		ok &= expect(parsed_metric.has_value() &&
		                 howdy::native::FaceMetricSpelling(*parsed_metric) == choice,
		             "typed metric reader accepts schema choice");
	}
	const auto normalized_metric_path = temp_root / "normalized-metric.ini";
	ok &= expect(write_file(normalized_metric_path, "[face]\nsface_metric = L2NORM\n"),
	             "write normalized metric");
	howdy::native::ConfigReader normalized_metric(normalized_metric_path.string());
	ok &= expect(howdy::native::ReadRuntimeString(normalized_metric, OptionId::kFaceSfaceMetric) ==
	                 "l2norm",
	             "generic string reader normalizes schema choice");
	const auto invalid_metric_path = temp_root / "invalid-metric-reader.ini";
	ok &= expect(write_file(invalid_metric_path, "[face]\nsface_metric = invalid\n"),
	             "write invalid metric");
	howdy::native::ConfigReader invalid_metric(invalid_metric_path.string());
	ok &= expect(howdy::native::ReadRuntimeString(invalid_metric, OptionId::kFaceSfaceMetric) ==
	                 howdy::native::config_schema::RuntimeDefaultString(OptionId::kFaceSfaceMetric),
	             "generic string reader falls back for invalid choice");
	ok &= expect(!howdy::native::ReadSfaceMetric(invalid_metric).has_value(),
	             "typed metric reader rejects unknown choice");
	const auto free_form_path = temp_root / "free-form-string.ini";
	ok &= expect(write_file(free_form_path, "[video]\ndevice_path = /dev/video0\n"),
	             "write free-form string");
	howdy::native::ConfigReader free_form(free_form_path.string());
	ok &= expect(howdy::native::ReadRuntimeString(free_form, OptionId::kVideoDevicePath) ==
	                 "/dev/video0",
	             "generic string reader preserves special free-form path");

	ok &= expect(Near(howdy::native::ParseConfigFloatStrict("1.25").value_or(0.0F), 1.25F),
	             "strict float parser accepts dot decimal");
	ok &= expect(Near(howdy::native::ParseConfigFloatStrict("+1.25").value_or(0.0F), 1.25F),
	             "strict float parser accepts leading plus");
	for (const auto *const value :
	     {"1,25", "1.25abc", "nan", "inf", "+inf", "-inf", " 1.25", "1.25 "}) {
		ok &= expect(!howdy::native::ParseConfigFloatStrict(value).has_value(),
		             std::string("strict float parser rejects ") + value);
	}

	const auto default_floats_path = temp_root / "default-floats.ini";
	ok &= expect(write_file(default_floats_path, "[video]\n"
	                                             "clahe_clip_limit = 1.25\n"
	                                             "[face]\n"
	                                             "yunet_score_threshold = 0.8845\n"
	                                             "yunet_nms_threshold = 0.3\n"
	                                             "sface_threshold = 0.6942\n"),
	             "write default float config");

	auto expect_default_float_config = [&](const std::string &label) -> bool {
		bool                        defaults_ok = true;
		howdy::native::ConfigReader defaults(default_floats_path.string());
		defaults_ok &= expect(defaults.Ok(), label + ": default float config parses");
		defaults_ok &=
		    expect(Near(howdy::native::ParseConfigFloatStrict("1.25").value_or(0.0F), 1.25F),
		           label + ": strict float parser accepts dot decimal");
		defaults_ok &= expect(!howdy::native::ParseConfigFloatStrict("1,25").has_value(),
		                      label + ": strict float parser rejects comma decimal");
		defaults_ok &= expect(!howdy::native::ValidateRuntimeConfig(defaults).has_value(),
		                      label + ": default float config validates");
		defaults_ok &= expect(
		    Near(howdy::native::ReadRuntimeFloat(defaults, OptionId::kVideoClaheClipLimit), 1.25F),
		    label + ": clahe_clip_limit keeps dot decimal value");
		defaults_ok &= expect(
		    Near(howdy::native::ReadRuntimeFloat(defaults, OptionId::kFaceYunetScoreThreshold),
		         0.8845F),
		    label + ": yunet_score_threshold keeps dot decimal value");
		defaults_ok &= expect(
		    Near(howdy::native::ReadRuntimeFloat(defaults, OptionId::kFaceYunetNmsThreshold), 0.3F),
		    label + ": yunet_nms_threshold keeps dot decimal value");
		defaults_ok &= expect(
		    Near(howdy::native::ReadSfaceThreshold(defaults, howdy::native::FaceMetric::kCosine),
		         0.6942F),
		    label + ": sface_threshold keeps dot decimal value");
		return defaults_ok;
	};

	ok &= expect_default_float_config("C locale");

	const char *current_locale = std::setlocale(LC_ALL, nullptr);
	const auto  previous_locale =
	    current_locale == nullptr ? std::optional<std::string>() : std::string(current_locale);
	const auto previous_lc_all     = GetEnvValue("LC_ALL");
	const auto previous_lc_numeric = GetEnvValue("LC_NUMERIC");
	const auto previous_lang       = GetEnvValue("LANG");
	unsetenv("LC_ALL");
	setenv("LANG", "C", 1);
	setenv("LC_NUMERIC", "nl_NL.UTF-8", 1);
	if (std::setlocale(LC_ALL, "") != nullptr) {
		ok &= expect_default_float_config("LC_NUMERIC=nl_NL.UTF-8");
	} else {
		std::cerr << "SKIP: nl_NL.UTF-8 locale is not generated\n";
	}
	RestoreEnvValue("LC_ALL", previous_lc_all);
	RestoreEnvValue("LC_NUMERIC", previous_lc_numeric);
	RestoreEnvValue("LANG", previous_lang);
	if (previous_locale.has_value()) {
		std::setlocale(LC_ALL, previous_locale->c_str());
	}

	const std::vector<std::string> invalid_float_values = {"1,25", "1.25abc", "nan",
	                                                       "+inf", "-inf",    "inf"};
	for (const auto &value : invalid_float_values) {
		const auto invalid_float_path = temp_root / ("invalid-float-" + value + ".ini");
		ok &= expect(write_file(invalid_float_path, "[video]\n"
		                                            "clahe_clip_limit = " +
		                                                value + "\n"),
		             "write invalid float config for " + value);
		howdy::native::ConfigReader invalid_float(invalid_float_path.string());
		ok &= expect(invalid_float.Ok(), "invalid float config still parses for " + value);
		const auto validation = howdy::native::ValidateRuntimeConfig(invalid_float);
		ok &= expect(validation.has_value(), "invalid float validation rejects " + value);
		ok &= expect(
		    Near(howdy::native::ReadRuntimeFloat(invalid_float, OptionId::kVideoClaheClipLimit),
		         1.25F),
		    "invalid float getter falls back for " + value);
	}

	const auto                  missing_path = temp_root / "does-not-exist.ini";
	howdy::native::ConfigReader missing(missing_path.string());
	ok &= expect(!missing.Ok(), "missing config should fail parse");
	ok &= expect(missing.ParseError() != 0, "parse_error is non-zero for missing file");

	const auto invalid_path = temp_root / "invalid.ini";
	ok &= expect(write_file(invalid_path, "[face]\n"
	                                      "sface_metric = weird\n"
	                                      "sface_threshold = 9\n"
	                                      "yunet_score_threshold = 4\n"
	                                      "yunet_nms_threshold = 4\n"
	                                      "yunet_top_k = 0\n"
	                                      "[video]\n"
	                                      "timeout = 0\n"
	                                      "dark_threshold = 1000\n"
	                                      "max_height = 0\n"
	                                      "rotate = 3\n"
	                                      "exposure = -2\n"
	                                      "clahe_clip_limit = 0\n"
	                                      "clahe_tile_grid_size = 0\n"
	                                      "frame_width = 4\n"
	                                      "frame_height = 4\n"
	                                      "device_fps = 9999\n"),
	             "write invalid bounded config");
	howdy::native::ConfigReader invalid(invalid_path.string());
	ok &= expect(invalid.Ok(), "invalid bounded config still parses");
	ok &= expect(howdy::native::ValidateRuntimeConfig(invalid).has_value(),
	             "invalid bounded config fails semantic validation");
	ok &= expect(howdy::native::ReadRuntimeInt(invalid, OptionId::kVideoTimeout) == 4,
	             "invalid timeout falls back");
	ok &=
	    expect(Near(howdy::native::ReadRuntimeFloat(invalid, OptionId::kVideoDarkThreshold), 75.0F),
	           "invalid dark threshold falls back");
	ok &= expect(Near(howdy::native::ReadRuntimeFloat(invalid, OptionId::kVideoMaxHeight), 320.0F),
	             "invalid max height falls back");
	ok &= expect(howdy::native::ReadRuntimeInt(invalid, OptionId::kVideoRotate) == 0,
	             "invalid rotate falls back");
	ok &= expect(howdy::native::ReadRuntimeInt(invalid, OptionId::kVideoExposure) == -1,
	             "invalid exposure falls back");
	ok &= expect(
	    Near(howdy::native::ReadRuntimeFloat(invalid, OptionId::kVideoClaheClipLimit), 1.25F),
	    "invalid clahe clip limit falls back");
	ok &= expect(howdy::native::ReadRuntimeInt(invalid, OptionId::kVideoClaheTileGridSize) == 8,
	             "invalid clahe tile grid size falls back");
	ok &= expect(howdy::native::ReadRuntimeInt(invalid, OptionId::kVideoFrameWidth) == -1,
	             "invalid frame width falls back");
	ok &= expect(howdy::native::ReadRuntimeInt(invalid, OptionId::kVideoFrameHeight) == -1,
	             "invalid frame height falls back");
	ok &= expect(howdy::native::ReadRuntimeInt(invalid, OptionId::kVideoDeviceFps) == 0,
	             "invalid device fps falls back");
	ok &= expect(
	    Near(howdy::native::ReadRuntimeFloat(invalid, OptionId::kFaceYunetScoreThreshold), 0.8845F),
	    "invalid yunet score threshold falls back");
	ok &= expect(
	    Near(howdy::native::ReadRuntimeFloat(invalid, OptionId::kFaceYunetNmsThreshold), 0.3F),
	    "invalid yunet nms threshold falls back");
	ok &= expect(howdy::native::ReadRuntimeInt(invalid, OptionId::kFaceYunetTopK) == 1000,
	             "invalid yunet top k falls back");
	ok &= expect(howdy::native::ReadRuntimeString(invalid, OptionId::kFaceSfaceMetric) ==
	                 howdy::native::config_schema::RuntimeDefaultString(OptionId::kFaceSfaceMetric),
	             "invalid sface metric falls back");
	ok &=
	    expect(Near(howdy::native::ReadSfaceThreshold(invalid, howdy::native::FaceMetric::kCosine),
	                0.6942F),
	           "invalid sface threshold falls back");
	ok &= expect(
	    Near(howdy::native::ReadSfaceThreshold(invalid, howdy::native::FaceMetric::kL2), 0.6942F),
	    "invalid l2 sface threshold falls back");
	ok &= expect(howdy::native::IsAllowedCaptureDevicePath("/dev/video0"),
	             "video device path prefix is allowed");
	ok &= expect(howdy::native::IsAllowedCaptureDevicePath("/dev/v4l/by-path/platform-camera"),
	             "by-path device path prefix is allowed");
	ok &= expect(howdy::native::IsAllowedCaptureDevicePath("/dev/v4l/by-id/usb-camera"),
	             "by-id device path prefix is allowed");
	ok &= expect(howdy::native::IsAllowedCaptureDevicePath("none"),
	             "none device path remains allowed");
	ok &= expect(!howdy::native::IsAllowedCaptureDevicePath("/tmp/camera"),
	             "non-device path prefix is rejected");
	ok &= expect(!howdy::native::IsAllowedCaptureDevicePath("/dev/null"),
	             "wrong character device path is rejected");

	const auto malformed_path = temp_root / "malformed.ini";
	ok &= expect(write_file(malformed_path, "[video]\n"
	                                        "timeout = abc\n"
	                                        "dark_threshold = nope\n"
	                                        "[face]\n"
	                                        "sface_threshold = bad\n"),
	             "write malformed ini");
	howdy::native::ConfigReader malformed(malformed_path.string());
	ok &= expect(malformed.Ok(), "malformed config should still parse");
	ok &= expect(howdy::native::ValidateRuntimeConfig(malformed).has_value(),
	             "malformed numeric config fails semantic validation");
	ok &= expect(howdy::native::ReadRuntimeInt(malformed, OptionId::kVideoTimeout) == 4,
	             "malformed timeout falls back");
	ok &= expect(
	    Near(howdy::native::ReadRuntimeFloat(malformed, OptionId::kVideoDarkThreshold), 75.0F),
	    "malformed dark threshold falls back");
	ok &= expect(
	    Near(howdy::native::ReadSfaceThreshold(malformed, howdy::native::FaceMetric::kCosine),
	         0.6942F),
	    "malformed sface threshold falls back");

	const auto non_finite_path = temp_root / "non-finite.ini";
	ok &= expect(write_file(non_finite_path, "[video]\n"
	                                         "dark_threshold = nan\n"
	                                         "[face]\n"
	                                         "yunet_score_threshold = +inf\n"
	                                         "sface_threshold = -inf\n"),
	             "write non-finite numeric config");
	howdy::native::ConfigReader non_finite(non_finite_path.string());
	ok &= expect(non_finite.Ok(), "non-finite config should still parse");
	ok &= expect(howdy::native::ValidateRuntimeConfig(non_finite).has_value(),
	             "non-finite numeric values fail semantic validation");
	ok &= expect(
	    Near(howdy::native::ReadRuntimeFloat(non_finite, OptionId::kVideoDarkThreshold), 75.0F),
	    "non-finite float falls back to schema default");
	ok &=
	    expect(Near(howdy::native::ReadRuntimeFloat(non_finite, OptionId::kFaceYunetScoreThreshold),
	                0.8845F),
	           "infinite float falls back to schema default");
	ok &= expect(
	    Near(howdy::native::ReadSfaceThreshold(non_finite, howdy::native::FaceMetric::kCosine),
	         0.6942F),
	    "non-finite sface threshold falls back to schema default");

	const auto negative_fps_path = temp_root / "negative-fps.ini";
	ok &= expect(write_file(negative_fps_path, "[video]\n"
	                                           "device_fps = -1\n"),
	             "write negative fps config");
	howdy::native::ConfigReader negative_fps(negative_fps_path.string());
	ok &= expect(negative_fps.Ok(), "negative fps config should parse");
	const auto negative_fps_validation = howdy::native::ValidateRuntimeConfig(negative_fps);
	ok &= expect(negative_fps_validation.has_value(), "negative fps fails semantic validation");
	if (negative_fps_validation.has_value()) {
		ok &= expect(negative_fps_validation->contains("device_fps"),
		             "negative fps validation reports key name");
		ok &= expect(negative_fps_validation->contains("-1"),
		             "negative fps validation reports offending value");
	}
	ok &= expect(howdy::native::ReadRuntimeInt(negative_fps, OptionId::kVideoDeviceFps) == 0,
	             "negative fps still falls back to runtime default");

	const howdy::native::ConfigReader memory_with_path(
	    "/custom/config.ini", "[video]\ntimeout = 12\ndark_threshold = 42.0\n");
	ok &= expect(memory_with_path.Ok(), "ConfigReader(path, content) parses valid ini buffer");
	ok &= expect(memory_with_path.GetInt("video", "timeout", 3) == 12,
	             "ConfigReader(path, content) parses timeout value");
	ok &= expect(Near(memory_with_path.GetFloat("video", "dark_threshold", 0.0F), 42.0F),
	             "ConfigReader(path, content) parses dark_threshold value");

	fs::remove_all(temp_root, ec);
	if (!ok) {
		return 1;
	}
	return 0;
}
