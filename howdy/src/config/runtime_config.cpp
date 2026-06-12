#include "config/runtime_config.hpp"

#include "config/config_reader.hpp"
#include "config/config_utils.hpp"
#include "config/config_validation.hpp"
#include "config/config_values.hpp"

#include <utility>
#ifndef HOWDY_RUNTIME_CONFIG_EXPLICIT_PATH_ONLY
#	include "config/runtime_paths.hpp"
#endif

#include <cassert>
#include <string>

namespace howdy::native {
	namespace {

		auto failure_result(RuntimeConfigLoadStatus status, const std::filesystem::path &path,
		                    std::string error_message, int error_code = 0)
		    -> RuntimeConfigLoadResult {
			assert(status != RuntimeConfigLoadStatus::kOk);
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

		auto read_runtime_bool(const ConfigReader &reader, config_schema::OptionId id) -> bool {
			const auto &option = config_schema::runtime_config_option(id);
			return reader.get_bool(std::string(option.section), std::string(option.key),
			                       config_schema::runtime_default_bool(id));
		}

		auto read_runtime_string(const ConfigReader &reader, config_schema::OptionId id)
		    -> std::string {
			const auto &option = config_schema::runtime_config_option(id);
			return reader.get(std::string(option.section), std::string(option.key),
			                  std::string(config_schema::runtime_default_string(id)));
		}

		auto populate_runtime_config(const ConfigReader &reader) -> RuntimeConfig {
			using enum config_schema::OptionId;

			RuntimeConfig config;

			config.core.detection_notice    = read_runtime_bool(reader, core_detection_notice);
			config.core.no_confirmation     = read_runtime_bool(reader, core_no_confirmation);
			config.core.abort_if_ssh        = read_runtime_bool(reader, core_abort_if_ssh);
			config.core.abort_if_lid_closed = read_runtime_bool(reader, core_abort_if_lid_closed);
			config.core.disabled            = read_runtime_bool(reader, core_disabled);

			config.video.timeout              = config_timeout_seconds(reader);
			config.video.device_path          = read_runtime_string(reader, video_device_path);
			config.video.warn_no_device       = read_runtime_bool(reader, video_warn_no_device);
			config.video.max_height           = config_max_height(reader);
			config.video.frame_width          = config_frame_width(reader);
			config.video.frame_height         = config_frame_height(reader);
			config.video.clahe_enabled        = read_runtime_bool(reader, video_clahe_enabled);
			config.video.clahe_clip_limit     = config_clahe_clip_limit(reader);
			config.video.clahe_tile_grid_size = config_clahe_tile_grid_size(reader);
			config.video.dark_threshold       = config_dark_threshold(reader);
			config.video.force_mjpeg          = read_runtime_bool(reader, video_force_mjpeg);
			config.video.exposure             = config_exposure(reader);
			config.video.device_fps           = config_device_fps(reader);
			config.video.rotate               = config_rotate_mode(reader);

			config.face.yunet_model           = read_runtime_string(reader, face_yunet_model);
			config.face.sface_model           = read_runtime_string(reader, face_sface_model);
			config.face.yunet_score_threshold = config_yunet_score_threshold(reader);
			config.face.yunet_nms_threshold   = config_yunet_nms_threshold(reader);
			config.face.yunet_top_k           = config_yunet_top_k(reader);
			config.face.sface_metric          = config_sface_metric(reader);
			config.face.sface_threshold = config_sface_threshold(reader, config.face.sface_metric);

			config.snapshots.save_failed     = read_runtime_bool(reader, snapshots_save_failed);
			config.snapshots.save_successful = read_runtime_bool(reader, snapshots_save_successful);
			config.debug.end_report          = read_runtime_bool(reader, debug_end_report);

			return config;
		}

	}  // namespace

	auto load_runtime_config(const std::filesystem::path &config_path,
	                         const std::optional<uid_t>   owner_uid) -> RuntimeConfigLoadResult {
		const auto security = check_secure_config_path(config_path, owner_uid);
		if (!security.ok) {
			return failure_result(RuntimeConfigLoadStatus::kPathError, config_path,
			                      security.error_message, security.error_code);
		}

		const ConfigReader reader(config_path.string());
		if (!reader.ok()) {
			return failure_result(RuntimeConfigLoadStatus::kParseError, config_path,
			                      "Failed to parse config: " + config_path.string() + " (error " +
			                          std::to_string(reader.parse_error()) + ")");
		}

		if (const auto validation = validate_runtime_config(reader)) {
			return failure_result(RuntimeConfigLoadStatus::kInvalidRuntimeValue, config_path,
			                      "Invalid runtime config in " + config_path.string() + ": " +
			                          *validation);
		}

		return success_result(config_path, populate_runtime_config(reader));
	}

#ifndef HOWDY_RUNTIME_CONFIG_EXPLICIT_PATH_ONLY
	auto load_runtime_config(const std::filesystem::path &config_path) -> RuntimeConfigLoadResult {
		return load_runtime_config(config_path, default_secure_owner_uid());
	}

	auto load_runtime_config() -> RuntimeConfigLoadResult {
		return load_runtime_config(resolve_config_path());
	}
#endif

}  // namespace howdy::native
