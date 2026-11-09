#pragma once

#include "support/face_metric.hpp"

#include <string>

namespace howdy::native {

	struct CoreConfig {
		bool detection_notice{};
		bool no_confirmation{};
		bool abort_if_ssh{};
		bool abort_if_lid_closed{};
		bool disabled{};

		friend auto operator==(const CoreConfig &, const CoreConfig &) -> bool = default;
	};

	auto DefaultCoreConfig() -> CoreConfig;

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

		friend auto operator==(const VideoConfig &, const VideoConfig &) -> bool = default;
	};

	auto DefaultVideoConfig() -> VideoConfig;
	auto DefaultFaceMetric() -> FaceMetric;

	struct FaceConfig {
		float      yunet_score_threshold{};
		float      yunet_nms_threshold{};
		int        yunet_top_k{};
		FaceMetric sface_metric = DefaultFaceMetric();
		float      sface_threshold{};

		friend auto operator==(const FaceConfig &, const FaceConfig &) -> bool = default;
	};

	auto DefaultFaceConfig() -> FaceConfig;

	struct DebugConfig {
		bool end_report{};

		friend auto operator==(const DebugConfig &, const DebugConfig &) -> bool = default;
	};

	auto DefaultDebugConfig() -> DebugConfig;

	struct RuntimeConfig {
		CoreConfig  core  = DefaultCoreConfig();
		VideoConfig video = DefaultVideoConfig();
		FaceConfig  face  = DefaultFaceConfig();
		DebugConfig debug = DefaultDebugConfig();

		friend auto operator==(const RuntimeConfig &, const RuntimeConfig &) -> bool = default;
	};

}  // namespace howdy::native
