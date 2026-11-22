#include "config/config_schema.hpp"
#include "config/runtime_config.hpp"
#include "config/runtime_config_loader.hpp"
#include "support/face_metric.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include <sys/stat.h>

namespace {

	using howdy::test::CreateTempDirectory;
	using howdy::test::Expect;
	using howdy::test::ExpectNear;
	using howdy::test::TemporaryDirectory;
	using howdy::test::WriteFile;

	auto LoadConfig(const std::filesystem::path &root, const std::string &filename,
	                std::string_view content) -> howdy::native::RuntimeConfigLoadResult {
		const auto path = root / filename;
		if (!WriteFile(path, content)) {
			return {
			    .ok            = false,
			    .status        = howdy::native::RuntimeConfigLoadStatus::kPathError,
			    .path          = path,
			    .config        = std::nullopt,
			    .error_message = "failed to create test file",
			    .error_code    = 0,
			};
		}

		if (chmod(path.c_str(), 0644) != 0) {
			return {
			    .ok            = false,
			    .status        = howdy::native::RuntimeConfigLoadStatus::kPathError,
			    .path          = path,
			    .config        = std::nullopt,
			    .error_message = "failed to chmod test file",
			    .error_code    = 0,
			};
		}

		return howdy::native::LoadRuntimeConfig(path, std::nullopt, {root});
	}

}  // namespace

auto main() -> int {
	bool ok = true;

	const auto temp_directory = CreateTempDirectory("howdy-runtime-config-test-");
	if (!temp_directory.has_value()) {
		std::cerr << "FAIL: create temp directory\n";
		return 1;
	}
	const TemporaryDirectory temp_directory_guard(*temp_directory);
	const auto              &root = temp_directory_guard.Path();

	using enum howdy::native::config_schema::OptionId;
	const howdy::native::RuntimeConfig defaults;
	const auto expect_bool_default = [&](bool actual, howdy::native::config_schema::OptionId id,
	                                     const std::string &message) -> bool {
		return Expect(actual == howdy::native::config_schema::RuntimeDefaultBool(id), message);
	};
	const auto expect_int_default = [&](int actual, howdy::native::config_schema::OptionId id,
	                                    const std::string &message) -> bool {
		return Expect(actual == howdy::native::config_schema::RuntimeDefaultInt(id), message);
	};
	const auto expect_float_default = [&](float actual, howdy::native::config_schema::OptionId id,
	                                      const std::string &message) -> bool {
		return ExpectNear(actual, howdy::native::config_schema::RuntimeDefaultFloat(id), 0.0001F,
		                  message);
	};
	const auto expect_string_default = [&](const std::string                     &actual,
	                                       howdy::native::config_schema::OptionId id,
	                                       const std::string                     &message) -> bool {
		return Expect(actual == howdy::native::config_schema::RuntimeDefaultString(id), message);
	};
	const auto expect_video_defaults = [&](const howdy::native::VideoConfig &video,
	                                       const std::string                &source) -> bool {
		bool matches = true;
		matches &=
		    expect_int_default(video.timeout, kVideoTimeout, source + " timeout matches schema");
		matches &= expect_string_default(video.device_path, kVideoDevicePath,
		                                 source + " device_path matches schema");
		matches &= expect_bool_default(video.warn_no_device, kVideoWarnNoDevice,
		                               source + " warn_no_device matches schema");
		matches &= expect_float_default(video.max_height, kVideoMaxHeight,
		                                source + " max_height matches schema");
		matches &= expect_int_default(video.frame_width, kVideoFrameWidth,
		                              source + " frame_width matches schema");
		matches &= expect_int_default(video.frame_height, kVideoFrameHeight,
		                              source + " frame_height matches schema");
		matches &= expect_bool_default(video.clahe_enabled, kVideoClaheEnabled,
		                               source + " clahe_enabled matches schema");
		matches &= expect_float_default(video.clahe_clip_limit, kVideoClaheClipLimit,
		                                source + " clahe_clip_limit matches schema");
		matches &= expect_int_default(video.clahe_tile_grid_size, kVideoClaheTileGridSize,
		                              source + " clahe_tile_grid_size matches schema");
		matches &= expect_float_default(video.dark_threshold, kVideoDarkThreshold,
		                                source + " dark_threshold matches schema");
		matches &= expect_bool_default(video.force_mjpeg, kVideoForceMjpeg,
		                               source + " force_mjpeg matches schema");
		matches &=
		    expect_int_default(video.exposure, kVideoExposure, source + " exposure matches schema");
		matches &= expect_int_default(video.device_fps, kVideoDeviceFps,
		                              source + " device_fps matches schema");
		matches &=
		    expect_int_default(video.rotate, kVideoRotate, source + " rotate matches schema");
		return matches;
	};
	const auto expect_face_defaults = [&](const howdy::native::FaceConfig &face,
	                                      const std::string               &source) -> bool {
		bool matches = true;
		matches &= expect_float_default(face.yunet_score_threshold, kFaceYunetScoreThreshold,
		                                source + " yunet_score_threshold matches schema");
		matches &= expect_float_default(face.yunet_nms_threshold, kFaceYunetNmsThreshold,
		                                source + " yunet_nms_threshold matches schema");
		matches &= expect_int_default(face.yunet_top_k, kFaceYunetTopK,
		                              source + " yunet_top_k matches schema");
		matches &=
		    Expect(face.sface_metric == howdy::native::config_schema::kSfaceDefaultMetric &&
			           howdy::native::FaceMetricSpelling(face.sface_metric) ==
			               howdy::native::config_schema::RuntimeDefaultString(kFaceSfaceMetric),
			       source + " sface_metric matches schema");
		matches &= expect_float_default(face.sface_threshold, kFaceSfaceThreshold,
		                                source + " sface_threshold matches schema");
		return matches;
	};

	ok &= expect_bool_default(defaults.core.detection_notice, kCoreDetectionNotice,
	                          "core detection_notice struct default matches schema");
	ok &= expect_bool_default(defaults.core.no_confirmation, kCoreNoConfirmation,
	                          "core no_confirmation struct default matches schema");
	ok &= expect_bool_default(defaults.core.abort_if_ssh, kCoreAbortIfSsh,
	                          "core abort_if_ssh struct default matches schema");
	ok &= expect_bool_default(defaults.core.abort_if_lid_closed, kCoreAbortIfLidClosed,
	                          "core abort_if_lid_closed struct default matches schema");
	ok &= expect_bool_default(defaults.core.disabled, kCoreDisabled,
	                          "core disabled struct default matches schema");
	ok &= expect_video_defaults(howdy::native::DefaultVideoConfig(), "factory");
	ok &= expect_video_defaults(defaults.video, "RuntimeConfig default");
	ok &= expect_face_defaults(howdy::native::DefaultFaceConfig(), "factory");
	ok &= expect_face_defaults(defaults.face, "RuntimeConfig default");
	ok &= expect_bool_default(defaults.debug.end_report, kDebugEndReport,
	                          "debug end_report struct default matches schema");

	const auto minimal = LoadConfig(root, "minimal.ini", "[core]\n");
	ok &= Expect(minimal.ok, "minimal config loads");
	ok &= Expect(minimal.config.has_value(), "minimal config has config");
	if (minimal.config.has_value()) {
		const auto &config = *minimal.config;
		ok &= Expect(config.core.detection_notice ==
		                 howdy::native::config_schema::RuntimeDefaultBool(kCoreDetectionNotice),
		             "core default loads from schema");
		ok &= expect_video_defaults(config.video, "minimal config");
		ok &= ExpectNear(config.face.sface_threshold,
		                 howdy::native::config_schema::RuntimeDefaultFloat(kFaceSfaceThreshold),
		                 0.0001F, "cosine threshold default loads from schema");
	}

	const auto custom = LoadConfig(root, "custom.ini",
	                               "[core]\n"
	                               "no_confirmation = false\n"
	                               "detection_notice = true\n"
	                               "abort_if_ssh = false\n"
	                               "abort_if_lid_closed = true\n"
	                               "disabled = false\n"
	                               "[video]\n"
	                               "timeout = 12\n"
	                               "device_path = none\n"
	                               "warn_no_device = false\n"
	                               "max_height = 640\n"
	                               "frame_width = 1280\n"
	                               "frame_height = 720\n"
	                               "clahe_enabled = false\n"
	                               "clahe_clip_limit = 2.5\n"
	                               "clahe_tile_grid_size = 13\n"
	                               "dark_threshold = 42.5\n"
	                               "force_mjpeg = true\n"
	                               "exposure = 20\n"
	                               "device_fps = 30\n"
	                               "rotate = 2\n"
	                               "[face]\n"
	                               "yunet_score_threshold = 0.8\n"
	                               "yunet_nms_threshold = 0.2\n"
	                               "yunet_top_k = 1234\n"
	                               "sface_metric = l2\n"
	                               "sface_threshold = 3.5\n"
	                               "[debug]\n"
	                               "end_report = true\n");
	ok &= Expect(custom.ok, "custom config loads");
	ok &= Expect(custom.config.has_value(), "custom config has config");
	if (custom.config.has_value()) {
		const auto &config = *custom.config;
		ok &= Expect(config.core.detection_notice && !config.core.no_confirmation &&
		                 !config.core.abort_if_ssh && config.core.abort_if_lid_closed &&
		                 !config.core.disabled,
		             "custom core fields map to their schema options");
		ok &= Expect(config.video.timeout == 12 && config.video.device_path == "none" &&
		                 !config.video.warn_no_device && config.video.frame_width == 1280 &&
		                 config.video.frame_height == 720 && !config.video.clahe_enabled &&
		                 config.video.clahe_tile_grid_size == 13 && config.video.force_mjpeg &&
		                 config.video.exposure == 20 && config.video.device_fps == 30 &&
		                 config.video.rotate == 2,
		             "custom video fields map to their schema options");
		ok &= ExpectNear(config.video.max_height, 640.0F, 0.0001F,
		                 "custom video max_height maps to its schema option");
		ok &= ExpectNear(config.video.clahe_clip_limit, 2.5F, 0.0001F,
		                 "custom video clahe_clip_limit maps to its schema option");
		ok &= ExpectNear(config.video.dark_threshold, 42.5F, 0.0001F,
		                 "custom video dark_threshold maps to its schema option");
		ok &= Expect(config.face.yunet_top_k == 1234 &&
		                 config.face.sface_metric == howdy::native::FaceMetric::kL2,
		             "custom face fields map to their schema options");
		ok &= ExpectNear(config.face.yunet_score_threshold, 0.8F, 0.0001F,
		                 "custom face yunet_score_threshold maps to its schema option");
		ok &= ExpectNear(config.face.yunet_nms_threshold, 0.2F, 0.0001F,
		                 "custom face yunet_nms_threshold maps to its schema option");
		ok &= ExpectNear(config.face.sface_threshold, 3.5F, 0.0001F,
		                 "custom face sface_threshold maps to its schema option");
		ok &= Expect(config.debug.end_report, "custom debug field maps to its schema option");
	}

	const auto upside_down = LoadConfig(root, "rotate-180.ini", "[video]\nrotate = 3\n");
	ok &= Expect(upside_down.ok && upside_down.config.has_value() &&
	                 upside_down.config->video.rotate == 3,
	             "rotate 3 loads as a runtime video option");

	const auto boolean_mapping =
	    LoadConfig(root, "boolean-mapping.ini",
		           "[core]\nno_confirmation = true\nabort_if_ssh = false\n"
		           "[video]\nwarn_no_device = true\nclahe_enabled = false\n");
	ok &= Expect(boolean_mapping.ok && boolean_mapping.config.has_value(),
	             "same-type boolean mapping config loads");
	if (boolean_mapping.config.has_value()) {
		const auto &config = *boolean_mapping.config;
		ok &= Expect(config.core.no_confirmation && !config.core.abort_if_ssh &&
		                 config.video.warn_no_device && !config.video.clahe_enabled,
		             "same-type boolean fields map to distinct schema options");
	}

	const auto cosine =
	    LoadConfig(root, "cosine.ini", "[face]\nsface_metric = COSINE\nsface_threshold = 0.5\n");
	ok &= Expect(cosine.ok, "cosine config loads");
	ok &= Expect(cosine.config.has_value(), "cosine config has config");
	if (cosine.config.has_value()) {
		const auto &config = *cosine.config;
		ok &= Expect(config.face.sface_metric == howdy::native::FaceMetric::kCosine,
		             "cosine metric normalizes");
	}

	const auto invalid_cosine = LoadConfig(
	    root, "invalid-cosine.ini", "[face]\nsface_metric = cosine\nsface_threshold = 1.1\n");
	ok &= Expect(!invalid_cosine.ok && invalid_cosine.error_message.contains("sface_threshold"),
	             "cosine threshold above one fails");
	ok &= Expect(!invalid_cosine.config.has_value(), "invalid cosine config has no config");

	const auto l2 = LoadConfig(root, "l2.ini", "[face]\nsface_metric = l2\nsface_threshold = 4\n");
	ok &= Expect(l2.ok, "l2 config loads");
	ok &= Expect(l2.config.has_value(), "l2 config has config");
	if (l2.config.has_value()) {
		const auto &config = *l2.config;
		ok &= Expect(config.face.sface_metric == howdy::native::FaceMetric::kL2,
		             "l2 threshold up to four selects l2 metric");
		ok &=
		    ExpectNear(config.face.sface_threshold, 4.0F, 0.0001F, "l2 threshold up to four loads");
	}

	const auto sentinel =
	    LoadConfig(root, "sentinel.ini", "[video]\nframe_width = -1\nexposure = -1\n");
	ok &= Expect(sentinel.ok && sentinel.config.has_value(), "allowed video sentinels load");
	if (sentinel.config.has_value()) {
		const auto &config = *sentinel.config;
		ok &= Expect(config.video.frame_width == -1 && config.video.exposure == -1,
		             "allowed video sentinels map to correct fields");
	}

	const auto invalid_fps = LoadConfig(root, "invalid-fps.ini", "[video]\ndevice_fps = -1\n");
	ok &= Expect(!invalid_fps.ok && invalid_fps.error_message.contains("device_fps"),
	             "negative device fps fails");
	ok &= Expect(!invalid_fps.config.has_value(), "invalid fps config has no config");

	const auto invalid_float =
	    LoadConfig(root, "invalid-float.ini", "[video]\ndark_threshold = fast\n");
	ok &= Expect(!invalid_float.ok && invalid_float.error_message.contains("dark_threshold"),
	             "invalid float text fails");
	ok &= Expect(!invalid_float.config.has_value(), "invalid float config has no config");

	const auto empty = LoadConfig(root, "empty.ini",
	                              "[core]\ndisabled = \n"
	                              "[video]\ntimeout = \ndark_threshold = \n"
	                              "[face]\nsface_metric = \nsface_threshold = \n");
	ok &= Expect(empty.ok, "empty values load");
	ok &= Expect(empty.config.has_value(), "empty config has config");
	if (empty.config.has_value()) {
		const auto &config = *empty.config;
		ok &= Expect(!config.core.disabled &&
		                 config.video.timeout ==
		                     howdy::native::config_schema::RuntimeDefaultInt(kVideoTimeout),
		             "empty values preserve schema core and video timeout defaults");
		ok &= ExpectNear(config.video.dark_threshold,
		                 howdy::native::config_schema::RuntimeDefaultFloat(kVideoDarkThreshold),
		                 0.0001F, "empty values preserve schema dark_threshold default");
		ok &= Expect(config.face.sface_metric == howdy::native::config_schema::kSfaceDefaultMetric,
		             "empty face metric preserves schema default");
		ok &= ExpectNear(config.face.sface_threshold,
		                 howdy::native::config_schema::RuntimeDefaultFloat(kFaceSfaceThreshold),
		                 0.0001F, "empty face sface_threshold preserves schema default");
	}

	return ok ? 0 : 1;
}
