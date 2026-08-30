#include "config/config_schema.hpp"
#include "config/runtime_config.hpp"
#include "test_support.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

	using howdy::test::expect;
	using howdy::test::write_file;

	struct TemporaryDirectory {
		std::filesystem::path path;

		explicit TemporaryDirectory(std::filesystem::path directory_path)
		    : path(std::move(directory_path)) {}

		~TemporaryDirectory() {
			std::error_code ec;
			std::filesystem::remove_all(path, ec);
		}

		TemporaryDirectory(const TemporaryDirectory &)                     = delete;
		auto operator=(const TemporaryDirectory &) -> TemporaryDirectory & = delete;
		TemporaryDirectory(TemporaryDirectory &&)                          = delete;
		auto operator=(TemporaryDirectory &&) -> TemporaryDirectory &      = delete;
	};

	auto nearly_equal(float lhs, float rhs) -> bool {
		return std::abs(lhs - rhs) < 0.0001F;
	}

	auto create_temp_directory() -> std::optional<std::filesystem::path> {
		std::error_code ec;
		const auto      temp_root =
		    std::filesystem::temp_directory_path(ec) / "howdy-runtime-config-test-XXXXXX";
		if (ec) {
			return std::nullopt;
		}

		std::string template_path = temp_root.string();
		char       *created       = mkdtemp(template_path.data());
		if (created == nullptr) {
			return std::nullopt;
		}

		return std::filesystem::path(created);
	}

	auto load_config(const std::filesystem::path &root, const std::string &filename,
	                 std::string_view content) -> howdy::native::RuntimeConfigLoadResult {
		const auto path = root / filename;
		if (!write_file(path, content)) {
			return {
			    .ok            = false,
			    .status        = howdy::native::RuntimeConfigLoadStatus::kPathError,
			    .path          = path,
			    .config        = std::nullopt,
			    .error_message = "failed to create test file",
			    .error_code    = 0,
			};
		}

		return howdy::native::load_runtime_config(path, std::nullopt);
	}

}  // namespace

auto main() -> int {
	bool ok = true;

	const auto temp_directory = create_temp_directory();
	if (!temp_directory.has_value()) {
		std::cerr << "FAIL: create temp directory\n";
		return 1;
	}
	const TemporaryDirectory temp_directory_guard(*temp_directory);
	const auto              &root = temp_directory_guard.path;

	using enum howdy::native::config_schema::OptionId;
	const howdy::native::RuntimeConfig defaults;
	const auto expect_bool_default = [&](bool actual, howdy::native::config_schema::OptionId id,
	                                     const std::string &message) -> bool {
		return expect(actual == howdy::native::config_schema::runtime_default_bool(id), message);
	};
	const auto expect_int_default = [&](int actual, howdy::native::config_schema::OptionId id,
	                                    const std::string &message) -> bool {
		return expect(actual == howdy::native::config_schema::runtime_default_int(id), message);
	};
	const auto expect_float_default = [&](float actual, howdy::native::config_schema::OptionId id,
	                                      const std::string &message) -> bool {
		return expect(nearly_equal(actual, howdy::native::config_schema::runtime_default_float(id)),
		              message);
	};
	const auto expect_string_default = [&](const std::string                     &actual,
	                                       howdy::native::config_schema::OptionId id,
	                                       const std::string                     &message) -> bool {
		return expect(actual == howdy::native::config_schema::runtime_default_string(id), message);
	};
	const auto expect_video_defaults = [&](const howdy::native::VideoConfig &video,
	                                       const std::string                &source) -> bool {
		bool matches = true;
		matches &=
		    expect_int_default(video.timeout, video_timeout, source + " timeout matches schema");
		matches &= expect_string_default(video.device_path, video_device_path,
		                                 source + " device_path matches schema");
		matches &= expect_bool_default(video.warn_no_device, video_warn_no_device,
		                               source + " warn_no_device matches schema");
		matches &= expect_float_default(video.max_height, video_max_height,
		                                source + " max_height matches schema");
		matches &= expect_int_default(video.frame_width, video_frame_width,
		                              source + " frame_width matches schema");
		matches &= expect_int_default(video.frame_height, video_frame_height,
		                              source + " frame_height matches schema");
		matches &= expect_bool_default(video.clahe_enabled, video_clahe_enabled,
		                               source + " clahe_enabled matches schema");
		matches &= expect_float_default(video.clahe_clip_limit, video_clahe_clip_limit,
		                                source + " clahe_clip_limit matches schema");
		matches &= expect_int_default(video.clahe_tile_grid_size, video_clahe_tile_grid_size,
		                              source + " clahe_tile_grid_size matches schema");
		matches &= expect_float_default(video.dark_threshold, video_dark_threshold,
		                                source + " dark_threshold matches schema");
		matches &= expect_bool_default(video.force_mjpeg, video_force_mjpeg,
		                               source + " force_mjpeg matches schema");
		matches &=
		    expect_int_default(video.exposure, video_exposure, source + " exposure matches schema");
		matches &= expect_int_default(video.device_fps, video_device_fps,
		                              source + " device_fps matches schema");
		matches &=
		    expect_int_default(video.rotate, video_rotate, source + " rotate matches schema");
		return matches;
	};
	const auto expect_face_defaults = [&](const howdy::native::FaceConfig &face,
	                                      const std::string               &source) -> bool {
		bool matches = true;
		matches &= expect_float_default(face.yunet_score_threshold, face_yunet_score_threshold,
		                                source + " yunet_score_threshold matches schema");
		matches &= expect_float_default(face.yunet_nms_threshold, face_yunet_nms_threshold,
		                                source + " yunet_nms_threshold matches schema");
		matches &= expect_int_default(face.yunet_top_k, face_yunet_top_k,
		                              source + " yunet_top_k matches schema");
		matches &=
		    expect(face.sface_metric == howdy::native::config_schema::sface_default_metric &&
		               howdy::native::face_metric_spelling(face.sface_metric) ==
		                   howdy::native::config_schema::runtime_default_string(face_sface_metric),
		           source + " sface_metric matches schema");
		matches &= expect_float_default(face.sface_threshold, face_sface_threshold,
		                                source + " sface_threshold matches schema");
		return matches;
	};

	ok &= expect_bool_default(defaults.core.detection_notice, core_detection_notice,
	                          "core detection_notice struct default matches schema");
	ok &= expect_bool_default(defaults.core.no_confirmation, core_no_confirmation,
	                          "core no_confirmation struct default matches schema");
	ok &= expect_bool_default(defaults.core.abort_if_ssh, core_abort_if_ssh,
	                          "core abort_if_ssh struct default matches schema");
	ok &= expect_bool_default(defaults.core.abort_if_lid_closed, core_abort_if_lid_closed,
	                          "core abort_if_lid_closed struct default matches schema");
	ok &= expect_bool_default(defaults.core.disabled, core_disabled,
	                          "core disabled struct default matches schema");
	ok &= expect_video_defaults(howdy::native::default_video_config(), "factory");
	ok &= expect_video_defaults(defaults.video, "RuntimeConfig default");
	ok &= expect_face_defaults(howdy::native::default_face_config(), "factory");
	ok &= expect_face_defaults(defaults.face, "RuntimeConfig default");
	ok &= expect_bool_default(defaults.debug.end_report, debug_end_report,
	                          "debug end_report struct default matches schema");

	const auto minimal = load_config(root, "minimal.ini", "[core]\n");
	ok &= expect(minimal.ok, "minimal config loads");
	ok &= expect(minimal.config.has_value(), "minimal config has config");
	if (minimal.config.has_value()) {
		const auto &config = *minimal.config;
		ok &= expect(config.core.detection_notice ==
		                 howdy::native::config_schema::runtime_default_bool(core_detection_notice),
		             "core default loads from schema");
		ok &= expect_video_defaults(config.video, "minimal config");
		ok &= expect(
		    nearly_equal(config.face.sface_threshold,
		                 howdy::native::config_schema::runtime_default_float(face_sface_threshold)),
		    "cosine threshold default loads from schema");
	}

	const auto custom = load_config(root, "custom.ini",
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
	ok &= expect(custom.ok, "custom config loads");
	ok &= expect(custom.config.has_value(), "custom config has config");
	if (custom.config.has_value()) {
		const auto &config = *custom.config;
		ok &= expect(config.core.detection_notice && !config.core.no_confirmation &&
		                 !config.core.abort_if_ssh && config.core.abort_if_lid_closed &&
		                 !config.core.disabled,
		             "custom core fields map to their schema options");
		ok &= expect(
		    config.video.timeout == 12 && config.video.device_path == "none" &&
		        !config.video.warn_no_device && nearly_equal(config.video.max_height, 640.0F) &&
		        config.video.frame_width == 1280 && config.video.frame_height == 720 &&
		        !config.video.clahe_enabled && nearly_equal(config.video.clahe_clip_limit, 2.5F) &&
		        config.video.clahe_tile_grid_size == 13 &&
		        nearly_equal(config.video.dark_threshold, 42.5F) && config.video.force_mjpeg &&
		        config.video.exposure == 20 && config.video.device_fps == 30 &&
		        config.video.rotate == 2,
		    "custom video fields map to their schema options");
		ok &= expect(nearly_equal(config.face.yunet_score_threshold, 0.8F) &&
		                 nearly_equal(config.face.yunet_nms_threshold, 0.2F) &&
		                 config.face.yunet_top_k == 1234 &&
		                 config.face.sface_metric == howdy::native::FaceMetric::kL2 &&
		                 nearly_equal(config.face.sface_threshold, 3.5F),
		             "custom face fields map to their schema options");
		ok &= expect(config.debug.end_report, "custom debug field maps to its schema option");
	}

	const auto boolean_mapping =
	    load_config(root, "boolean-mapping.ini",
	                "[core]\nno_confirmation = true\nabort_if_ssh = false\n"
	                "[video]\nwarn_no_device = true\nclahe_enabled = false\n");
	ok &= expect(boolean_mapping.ok && boolean_mapping.config.has_value(),
	             "same-type boolean mapping config loads");
	if (boolean_mapping.config.has_value()) {
		const auto &config = *boolean_mapping.config;
		ok &= expect(config.core.no_confirmation && !config.core.abort_if_ssh &&
		                 config.video.warn_no_device && !config.video.clahe_enabled,
		             "same-type boolean fields map to distinct schema options");
	}

	const auto cosine =
	    load_config(root, "cosine.ini", "[face]\nsface_metric = COSINE\nsface_threshold = 0.5\n");
	ok &= expect(cosine.ok, "cosine config loads");
	ok &= expect(cosine.config.has_value(), "cosine config has config");
	if (cosine.config.has_value()) {
		const auto &config = *cosine.config;
		ok &= expect(config.face.sface_metric == howdy::native::FaceMetric::kCosine,
		             "cosine metric normalizes");
	}

	const auto invalid_cosine = load_config(
	    root, "invalid-cosine.ini", "[face]\nsface_metric = cosine\nsface_threshold = 1.1\n");
	ok &= expect(!invalid_cosine.ok && invalid_cosine.error_message.contains("sface_threshold"),
	             "cosine threshold above one fails");
	ok &= expect(!invalid_cosine.config.has_value(), "invalid cosine config has no config");

	const auto l2 = load_config(root, "l2.ini", "[face]\nsface_metric = l2\nsface_threshold = 4\n");
	ok &= expect(l2.ok, "l2 config loads");
	ok &= expect(l2.config.has_value(), "l2 config has config");
	if (l2.config.has_value()) {
		const auto &config = *l2.config;
		ok &= expect(config.face.sface_metric == howdy::native::FaceMetric::kL2 &&
		                 nearly_equal(config.face.sface_threshold, 4.0F),
		             "l2 threshold up to four loads");
	}

	const auto sentinel =
	    load_config(root, "sentinel.ini", "[video]\nframe_width = -1\nexposure = -1\n");
	ok &= expect(sentinel.ok && sentinel.config.has_value(), "allowed video sentinels load");
	if (sentinel.config.has_value()) {
		const auto &config = *sentinel.config;
		ok &= expect(config.video.frame_width == -1 && config.video.exposure == -1,
		             "allowed video sentinels map to correct fields");
	}

	const auto invalid_fps = load_config(root, "invalid-fps.ini", "[video]\ndevice_fps = -1\n");
	ok &= expect(!invalid_fps.ok && invalid_fps.error_message.contains("device_fps"),
	             "negative device fps fails");
	ok &= expect(!invalid_fps.config.has_value(), "invalid fps config has no config");

	const auto invalid_float =
	    load_config(root, "invalid-float.ini", "[video]\ndark_threshold = fast\n");
	ok &= expect(!invalid_float.ok && invalid_float.error_message.contains("dark_threshold"),
	             "invalid float text fails");
	ok &= expect(!invalid_float.config.has_value(), "invalid float config has no config");

	const auto empty = load_config(root, "empty.ini",
	                               "[core]\ndisabled = \n"
	                               "[video]\ntimeout = \ndark_threshold = \n"
	                               "[face]\nsface_metric = \nsface_threshold = \n");
	ok &= expect(empty.ok, "empty values load");
	ok &= expect(empty.config.has_value(), "empty config has config");
	if (empty.config.has_value()) {
		const auto &config = *empty.config;
		ok &= expect(!config.core.disabled &&
		                 config.video.timeout ==
		                     howdy::native::config_schema::runtime_default_int(video_timeout) &&
		                 nearly_equal(config.video.dark_threshold,
		                              howdy::native::config_schema::runtime_default_float(
		                                  video_dark_threshold)),
		             "empty values preserve schema defaults");
		ok &=
		    expect(config.face.sface_metric == howdy::native::config_schema::sface_default_metric &&
		               nearly_equal(config.face.sface_threshold,
		                            howdy::native::config_schema::runtime_default_float(
		                                face_sface_threshold)),
		           "empty face values preserve schema defaults");
	}

	return ok ? 0 : 1;
}
