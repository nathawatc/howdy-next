#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include <sys/types.h>

namespace howdy::native {

	struct CoreConfig {
		bool detection_notice{};
		bool no_confirmation{};
		bool abort_if_ssh{};
		bool abort_if_lid_closed{};
		bool disabled{};
	};

	struct VideoConfig {
		int         timeout{};
		std::string device_path;
		bool        warn_no_device{};
		float       max_height{};
		int         frame_width{};
		int         frame_height{};
		bool        clahe_enabled{};
		float       clahe_clip_limit{};
		int         clahe_tile_grid_size{};
		float       dark_threshold{};
		bool        force_mjpeg{};
		int         exposure{};
		int         device_fps{};
		int         rotate{};
	};

	auto default_video_config() -> VideoConfig;

	struct FaceConfig {
		float       yunet_score_threshold{};
		float       yunet_nms_threshold{};
		int         yunet_top_k{};
		std::string sface_metric;
		float       sface_threshold{};
	};

	auto default_face_config() -> FaceConfig;

	struct SnapshotConfig {
		bool save_failed{};
		bool save_successful{};
	};

	struct DebugConfig {
		bool end_report{};
	};

	struct RuntimeConfig {
		CoreConfig     core;
		VideoConfig    video;
		FaceConfig     face;
		SnapshotConfig snapshots;
		DebugConfig    debug;

		RuntimeConfig();
	};

	enum class RuntimeConfigLoadStatus : std::uint8_t {
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
