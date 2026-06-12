#pragma once

#include "config/config_schema.hpp"

#include <filesystem>
#include <optional>
#include <string>

#include <sys/types.h>

namespace howdy::native {

	struct CoreConfig {
		bool detection_notice =
		    config_schema::runtime_default_bool(config_schema::OptionId::core_detection_notice);
		bool no_confirmation =
		    config_schema::runtime_default_bool(config_schema::OptionId::core_no_confirmation);
		bool abort_if_ssh =
		    config_schema::runtime_default_bool(config_schema::OptionId::core_abort_if_ssh);
		bool abort_if_lid_closed =
		    config_schema::runtime_default_bool(config_schema::OptionId::core_abort_if_lid_closed);
		bool disabled = config_schema::runtime_default_bool(config_schema::OptionId::core_disabled);
	};

	struct VideoConfig {
		int timeout = config_schema::runtime_default_int(config_schema::OptionId::video_timeout);
		std::string device_path = std::string(
		    config_schema::runtime_default_string(config_schema::OptionId::video_device_path));
		bool warn_no_device =
		    config_schema::runtime_default_bool(config_schema::OptionId::video_warn_no_device);
		float max_height =
		    config_schema::runtime_default_float(config_schema::OptionId::video_max_height);
		int frame_width =
		    config_schema::runtime_default_int(config_schema::OptionId::video_frame_width);
		int frame_height =
		    config_schema::runtime_default_int(config_schema::OptionId::video_frame_height);
		bool clahe_enabled =
		    config_schema::runtime_default_bool(config_schema::OptionId::video_clahe_enabled);
		float clahe_clip_limit =
		    config_schema::runtime_default_float(config_schema::OptionId::video_clahe_clip_limit);
		int clahe_tile_grid_size =
		    config_schema::runtime_default_int(config_schema::OptionId::video_clahe_tile_grid_size);
		float dark_threshold =
		    config_schema::runtime_default_float(config_schema::OptionId::video_dark_threshold);
		bool force_mjpeg =
		    config_schema::runtime_default_bool(config_schema::OptionId::video_force_mjpeg);
		int exposure = config_schema::runtime_default_int(config_schema::OptionId::video_exposure);
		int device_fps =
		    config_schema::runtime_default_int(config_schema::OptionId::video_device_fps);
		int rotate = config_schema::runtime_default_int(config_schema::OptionId::video_rotate);
	};

	struct FaceConfig {
		std::string yunet_model = std::string(
		    config_schema::runtime_default_string(config_schema::OptionId::face_yunet_model));
		std::string sface_model = std::string(
		    config_schema::runtime_default_string(config_schema::OptionId::face_sface_model));
		float yunet_score_threshold = config_schema::runtime_default_float(
		    config_schema::OptionId::face_yunet_score_threshold);
		float yunet_nms_threshold =
		    config_schema::runtime_default_float(config_schema::OptionId::face_yunet_nms_threshold);
		int yunet_top_k =
		    config_schema::runtime_default_int(config_schema::OptionId::face_yunet_top_k);
		std::string sface_metric = std::string(
		    config_schema::runtime_default_string(config_schema::OptionId::face_sface_metric));
		float sface_threshold =
		    config_schema::runtime_default_float(config_schema::OptionId::face_sface_threshold);
	};

	struct SnapshotConfig {
		bool save_failed =
		    config_schema::runtime_default_bool(config_schema::OptionId::snapshots_save_failed);
		bool save_successful =
		    config_schema::runtime_default_bool(config_schema::OptionId::snapshots_save_successful);
	};

	struct DebugConfig {
		bool end_report =
		    config_schema::runtime_default_bool(config_schema::OptionId::debug_end_report);
	};

	struct RuntimeConfig {
		CoreConfig     core;
		VideoConfig    video;
		FaceConfig     face;
		SnapshotConfig snapshots;
		DebugConfig    debug;
	};

	enum class RuntimeConfigLoadStatus {
		kOk,
		kPathError,
		kParseError,
		kInvalidRuntimeValue,
	};

	struct RuntimeConfigLoadResult {
		bool                         ok     = false;
		RuntimeConfigLoadStatus      status = RuntimeConfigLoadStatus::kPathError;
		std::filesystem::path        path;
		std::optional<RuntimeConfig> config;
		std::string                  error_message;
		int                          error_code = 0;
	};

	auto load_runtime_config(const std::filesystem::path &config_path,
	                         std::optional<uid_t>         owner_uid) -> RuntimeConfigLoadResult;
#ifndef HOWDY_RUNTIME_CONFIG_EXPLICIT_PATH_ONLY
	auto load_runtime_config(const std::filesystem::path &config_path) -> RuntimeConfigLoadResult;
	auto load_runtime_config() -> RuntimeConfigLoadResult;
#endif

}  // namespace howdy::native
