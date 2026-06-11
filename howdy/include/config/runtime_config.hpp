#pragma once

#include "config/config_schema.hpp"

#include <filesystem>
#include <optional>
#include <string>

#include <sys/types.h>

namespace howdy::native {

	struct CoreConfig {
		bool detection_notice    = true;
		bool no_confirmation     = true;
		bool abort_if_ssh        = true;
		bool abort_if_lid_closed = true;
		bool disabled            = false;
	};

	struct VideoConfig {
		int         timeout              = 4;
		std::string device_path          = "/dev/video0";
		bool        warn_no_device       = true;
		float       max_height           = 320.0F;
		int         frame_width          = -1;
		int         frame_height         = -1;
		bool        clahe_enabled        = true;
		float       clahe_clip_limit     = 1.25F;
		int         clahe_tile_grid_size = 8;
		float       dark_threshold       = 60.0F;
		bool        force_mjpeg          = false;
		int         exposure             = -1;
		int         device_fps           = 0;
		int         rotate               = 0;
	};

	struct FaceConfig {
		std::string yunet_model;
		std::string sface_model;
		float       yunet_score_threshold = 0.9F;
		float       yunet_nms_threshold   = 0.3F;
		int         yunet_top_k           = 5000;
		std::string sface_metric          = "cosine";
		float       sface_threshold       = config_schema::sface_cosine_threshold_default;
	};

	struct SnapshotConfig {
		bool save_failed     = false;
		bool save_successful = false;
	};

	struct DebugConfig {
		bool end_report = false;
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
