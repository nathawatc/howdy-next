#include "config/runtime_config.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <sys/stat.h>

namespace {

	auto expect(bool condition, const std::string &message) -> bool {
		if (!condition) {
			std::cerr << "FAIL: " << message << "\n";
			return false;
		}
		return true;
	}

	auto nearly_equal(float left, float right) -> bool {
		return std::abs(left - right) < 0.0001F;
	}

	auto write_file(const std::filesystem::path &path, const std::string &content) -> bool {
		std::ofstream output(path);
		output << content;
		return output.good() && chmod(path.c_str(), 0644) == 0;
	}

	struct TemporaryDirectory {
		std::filesystem::path path;

		~TemporaryDirectory() {
			std::error_code ec;
			std::filesystem::remove_all(path, ec);
		}
	};

	auto create_temp_directory() -> std::optional<std::filesystem::path> {
		const auto template_path =
		    std::filesystem::temp_directory_path() / "howdy-runtime-config-test-XXXXXX";
		const auto        template_string = template_path.string();
		std::vector<char> path_buffer(template_string.begin(), template_string.end());
		path_buffer.push_back('\0');

		char *created = mkdtemp(path_buffer.data());
		if (created == nullptr || chmod(created, 0755) != 0) {
			return std::nullopt;
		}
		return std::filesystem::path(created);
	}

	auto load_config(const std::filesystem::path &root, const std::string &name,
	                 const std::string &content) -> howdy::native::RuntimeConfigLoadResult {
		const auto path = root / name;
		if (!write_file(path, content)) {
			return {};
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
	const TemporaryDirectory temp_directory_guard{.path = *temp_directory};
	const auto              &root = temp_directory_guard.path;

	using enum howdy::native::config_schema::OptionId;
	const howdy::native::RuntimeConfig defaults;
	const auto expect_bool_default = [&](bool actual, howdy::native::config_schema::OptionId id,
	                                     const std::string &message) {
		return expect(actual == howdy::native::config_schema::runtime_default_bool(id), message);
	};
	const auto expect_int_default = [&](int actual, howdy::native::config_schema::OptionId id,
	                                    const std::string &message) {
		return expect(actual == howdy::native::config_schema::runtime_default_int(id), message);
	};
	const auto expect_float_default = [&](float actual, howdy::native::config_schema::OptionId id,
	                                      const std::string &message) {
		return expect(nearly_equal(actual, howdy::native::config_schema::runtime_default_float(id)),
		              message);
	};
	const auto expect_string_default = [&](const std::string                     &actual,
	                                       howdy::native::config_schema::OptionId id,
	                                       const std::string                     &message) {
		return expect(actual == howdy::native::config_schema::runtime_default_string(id), message);
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
	ok &= expect_int_default(defaults.video.timeout, video_timeout,
	                         "video timeout struct default matches schema");
	ok &= expect_string_default(defaults.video.device_path, video_device_path,
	                            "video device_path struct default matches schema");
	ok &= expect_bool_default(defaults.video.warn_no_device, video_warn_no_device,
	                          "video warn_no_device struct default matches schema");
	ok &= expect_float_default(defaults.video.max_height, video_max_height,
	                           "video max_height struct default matches schema");
	ok &= expect_int_default(defaults.video.frame_width, video_frame_width,
	                         "video frame_width struct default matches schema");
	ok &= expect_int_default(defaults.video.frame_height, video_frame_height,
	                         "video frame_height struct default matches schema");
	ok &= expect_bool_default(defaults.video.clahe_enabled, video_clahe_enabled,
	                          "video clahe_enabled struct default matches schema");
	ok &= expect_float_default(defaults.video.clahe_clip_limit, video_clahe_clip_limit,
	                           "video clahe_clip_limit struct default matches schema");
	ok &= expect_int_default(defaults.video.clahe_tile_grid_size, video_clahe_tile_grid_size,
	                         "video clahe_tile_grid_size struct default matches schema");
	ok &= expect_float_default(defaults.video.dark_threshold, video_dark_threshold,
	                           "video dark_threshold struct default matches schema");
	ok &= expect_bool_default(defaults.video.force_mjpeg, video_force_mjpeg,
	                          "video force_mjpeg struct default matches schema");
	ok &= expect_int_default(defaults.video.exposure, video_exposure,
	                         "video exposure struct default matches schema");
	ok &= expect_int_default(defaults.video.device_fps, video_device_fps,
	                         "video device_fps struct default matches schema");
	ok &= expect_int_default(defaults.video.rotate, video_rotate,
	                         "video rotate struct default matches schema");
	ok &= expect_float_default(defaults.face.yunet_score_threshold, face_yunet_score_threshold,
	                           "face yunet_score_threshold struct default matches schema");
	ok &= expect_float_default(defaults.face.yunet_nms_threshold, face_yunet_nms_threshold,
	                           "face yunet_nms_threshold struct default matches schema");
	ok &= expect_int_default(defaults.face.yunet_top_k, face_yunet_top_k,
	                         "face yunet_top_k struct default matches schema");
	ok &= expect_string_default(defaults.face.sface_metric, face_sface_metric,
	                            "face sface_metric struct default matches schema");
	ok &= expect_float_default(defaults.face.sface_threshold, face_sface_threshold,
	                           "face sface_threshold struct default matches schema");
	ok &= expect_bool_default(defaults.snapshots.save_failed, snapshots_save_failed,
	                          "snapshots save_failed struct default matches schema");
	ok &= expect_bool_default(defaults.snapshots.save_successful, snapshots_save_successful,
	                          "snapshots save_successful struct default matches schema");
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
		ok &= expect(config.video.timeout ==
		                 howdy::native::config_schema::runtime_default_int(video_timeout),
		             "video default loads from schema");
		ok &= expect(config.video.device_path ==
		                 howdy::native::config_schema::runtime_default_string(video_device_path),
		             "device default loads from schema");
		ok &= expect(
		    nearly_equal(config.face.sface_threshold,
		                 howdy::native::config_schema::runtime_default_float(face_sface_threshold)),
		    "cosine threshold default loads from schema");
	}

	const auto custom = load_config(root, "custom.ini",
	                                "[core]\n"
	                                "no_confirmation = false\n"
	                                "detection_notice = false\n"
	                                "abort_if_ssh = false\n"
	                                "abort_if_lid_closed = false\n"
	                                "disabled = true\n"
	                                "[video]\n"
	                                "timeout = 12\n"
	                                "device_path = none\n"
	                                "warn_no_device = false\n"
	                                "max_height = 640\n"
	                                "frame_width = 1280\n"
	                                "frame_height = 720\n"
	                                "clahe_enabled = false\n"
	                                "clahe_clip_limit = 2.5\n"
	                                "clahe_tile_grid_size = 12\n"
	                                "dark_threshold = 42.5\n"
	                                "force_mjpeg = true\n"
	                                "exposure = 20\n"
	                                "device_fps = 30\n"
	                                "rotate = 2\n"
	                                "[face]\n"
	                                "yunet_score_threshold = 0.8\n"
	                                "yunet_nms_threshold = 0.2\n"
	                                "yunet_top_k = 1000\n"
	                                "sface_metric = l2\n"
	                                "sface_threshold = 3.5\n"
	                                "[snapshots]\n"
	                                "save_failed = true\n"
	                                "save_successful = true\n"
	                                "[debug]\n"
	                                "end_report = true\n");
	ok &= expect(custom.ok, "custom config loads");
	ok &= expect(custom.config.has_value(), "custom config has config");
	if (custom.config.has_value()) {
		const auto &config = *custom.config;
		ok &= expect(!config.core.no_confirmation && !config.core.detection_notice &&
		                 !config.core.abort_if_ssh && !config.core.abort_if_lid_closed &&
		                 config.core.disabled,
		             "PAM core values load");
		ok &= expect(config.video.timeout == 12 && config.video.device_path == "none",
		             "custom video values load");
		ok &= expect(nearly_equal(config.video.dark_threshold, 42.5F) &&
		                 config.video.device_fps == 30,
		             "custom bounded video values load");
		ok &= expect(config.face.sface_metric == "l2" &&
		                 nearly_equal(config.face.sface_threshold, 3.5F),
		             "custom face values load");
		ok &= expect(config.snapshots.save_failed && config.snapshots.save_successful &&
		                 config.debug.end_report,
		             "custom snapshot and debug values load");
	}

	const auto cosine =
	    load_config(root, "cosine.ini", "[face]\nsface_metric = COSINE\nsface_threshold = 0.5\n");
	ok &= expect(cosine.ok, "cosine config loads");
	ok &= expect(cosine.config.has_value(), "cosine config has config");
	if (cosine.config.has_value()) {
		const auto &config = *cosine.config;
		ok &= expect(config.face.sface_metric == "cosine", "cosine metric normalizes");
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
		ok &= expect(nearly_equal(config.face.sface_threshold, 4.0F),
		             "l2 threshold up to four loads");
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
		ok &= expect(
		    config.face.sface_metric ==
		            howdy::native::config_schema::runtime_default_string(face_sface_metric) &&
		        nearly_equal(
		            config.face.sface_threshold,
		            howdy::native::config_schema::runtime_default_float(face_sface_threshold)),
		    "empty face values preserve schema defaults");
	}

	return ok ? 0 : 1;
}
