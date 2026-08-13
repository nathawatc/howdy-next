#include "config/runtime_config.hpp"

#include "config/config_reader.hpp"
#include "config/config_utils.hpp"
#include "config/config_validation.hpp"
#include "config/config_values.hpp"

#include <string>
#include <utility>

namespace howdy::native {
	namespace {

		template <RuntimeConfigLoadStatus status>
		auto failure_result(const std::filesystem::path &path, std::string error_message,
		                    int error_code = 0) -> RuntimeConfigLoadResult {
			static_assert(status != RuntimeConfigLoadStatus::kOk);
			return RuntimeConfigLoadResult{
			    .ok            = false,
			    .status        = status,
			    .path          = path,
			    .error_message = std::move(error_message),
			    .error_code    = error_code,
			};
		}

		auto success_result(const std::filesystem::path &path, RuntimeConfig config)
		    -> RuntimeConfigLoadResult {
			return RuntimeConfigLoadResult{
			    .ok     = true,
			    .status = RuntimeConfigLoadStatus::kOk,
			    .path   = path,
			    .config = std::move(config),
			};
		}

		auto populate_runtime_config(const ConfigReader &reader) -> RuntimeConfig {
			using enum config_schema::OptionId;

			RuntimeConfig config;

			config.core.detection_notice    = read_runtime_bool(reader, core_detection_notice);
			config.core.no_confirmation     = read_runtime_bool(reader, core_no_confirmation);
			config.core.abort_if_ssh        = read_runtime_bool(reader, core_abort_if_ssh);
			config.core.abort_if_lid_closed = read_runtime_bool(reader, core_abort_if_lid_closed);
			config.core.disabled            = read_runtime_bool(reader, core_disabled);

			config.video.timeout          = read_runtime_int(reader, video_timeout);
			config.video.device_path      = read_runtime_string(reader, video_device_path);
			config.video.warn_no_device   = read_runtime_bool(reader, video_warn_no_device);
			config.video.max_height       = read_runtime_float(reader, video_max_height);
			config.video.frame_width      = read_runtime_int(reader, video_frame_width);
			config.video.frame_height     = read_runtime_int(reader, video_frame_height);
			config.video.clahe_enabled    = read_runtime_bool(reader, video_clahe_enabled);
			config.video.clahe_clip_limit = read_runtime_float(reader, video_clahe_clip_limit);
			config.video.clahe_tile_grid_size =
			    read_runtime_int(reader, video_clahe_tile_grid_size);
			config.video.dark_threshold = read_runtime_float(reader, video_dark_threshold);
			config.video.force_mjpeg    = read_runtime_bool(reader, video_force_mjpeg);
			config.video.exposure       = read_runtime_int(reader, video_exposure);
			config.video.device_fps     = read_runtime_int(reader, video_device_fps);
			config.video.rotate         = read_runtime_int(reader, video_rotate);

			config.face.yunet_score_threshold =
			    read_runtime_float(reader, face_yunet_score_threshold);
			config.face.yunet_nms_threshold = read_runtime_float(reader, face_yunet_nms_threshold);
			config.face.yunet_top_k         = read_runtime_int(reader, face_yunet_top_k);
			config.face.sface_metric        = read_runtime_string(reader, face_sface_metric);
			config.face.sface_threshold = read_sface_threshold(reader, config.face.sface_metric);

			config.snapshots.save_failed     = read_runtime_bool(reader, snapshots_save_failed);
			config.snapshots.save_successful = read_runtime_bool(reader, snapshots_save_successful);
			config.debug.end_report          = read_runtime_bool(reader, debug_end_report);

			return config;
		}

	}  // namespace

	auto default_video_config() -> VideoConfig {
		using enum config_schema::OptionId;

		return {
		    .timeout        = config_schema::runtime_default_int(video_timeout),
		    .device_path    = std::string(config_schema::runtime_default_string(video_device_path)),
		    .warn_no_device = config_schema::runtime_default_bool(video_warn_no_device),
		    .max_height     = config_schema::runtime_default_float(video_max_height),
		    .frame_width    = config_schema::runtime_default_int(video_frame_width),
		    .frame_height   = config_schema::runtime_default_int(video_frame_height),
		    .clahe_enabled  = config_schema::runtime_default_bool(video_clahe_enabled),
		    .clahe_clip_limit     = config_schema::runtime_default_float(video_clahe_clip_limit),
		    .clahe_tile_grid_size = config_schema::runtime_default_int(video_clahe_tile_grid_size),
		    .dark_threshold       = config_schema::runtime_default_float(video_dark_threshold),
		    .force_mjpeg          = config_schema::runtime_default_bool(video_force_mjpeg),
		    .exposure             = config_schema::runtime_default_int(video_exposure),
		    .device_fps           = config_schema::runtime_default_int(video_device_fps),
		    .rotate               = config_schema::runtime_default_int(video_rotate),
		};
	}

	auto load_runtime_config(const std::filesystem::path &config_path,
	                         const std::optional<uid_t>   owner_uid) -> RuntimeConfigLoadResult {
		const auto security = check_secure_config_path(config_path, owner_uid);
		if (!security.ok) {
			return failure_result<RuntimeConfigLoadStatus::kPathError>(
			    config_path, security.error_message, security.error_code);
		}

		const ConfigReader reader(config_path.string());
		if (!reader.ok()) {
			return failure_result<RuntimeConfigLoadStatus::kParseError>(
			    config_path, "Failed to parse config: " + config_path.string() + " (error " +
			                     std::to_string(reader.parse_error()) + ")");
		}

		if (const auto validation = validate_runtime_config(reader)) {
			return failure_result<RuntimeConfigLoadStatus::kInvalidRuntimeValue>(
			    config_path,
			    "Invalid runtime config in " + config_path.string() + ": " + *validation);
		}

		return success_result(config_path, populate_runtime_config(reader));
	}
}  // namespace howdy::native
