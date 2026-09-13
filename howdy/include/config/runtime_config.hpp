#pragma once

#include "config/config_schema.hpp"
#include "support/face_metric.hpp"

#include <string>

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

	auto DefaultVideoConfig() -> VideoConfig;

	struct FaceConfig {
		float      yunet_score_threshold{};
		float      yunet_nms_threshold{};
		int        yunet_top_k{};
		FaceMetric sface_metric = config_schema::kSfaceDefaultMetric;
		float      sface_threshold{};
	};

	auto DefaultFaceConfig() -> FaceConfig;

	struct DebugConfig {
		bool end_report{};
	};

	struct RuntimeConfig {
		CoreConfig  core;
		VideoConfig video;
		FaceConfig  face;
		DebugConfig debug;

		RuntimeConfig();
	};

}  // namespace howdy::native
