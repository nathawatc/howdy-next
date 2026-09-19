#include "config/runtime_config.hpp"

#include "config/config_schema.hpp"

#include <string>

namespace howdy::native {
	auto DefaultVideoConfig() -> VideoConfig {
		using enum config_schema::OptionId;
		return {
		    .timeout          = config_schema::RuntimeDefaultInt(kVideoTimeout),
		    .device_path      = std::string(config_schema::RuntimeDefaultString(kVideoDevicePath)),
		    .warn_no_device   = config_schema::RuntimeDefaultBool(kVideoWarnNoDevice),
		    .max_height       = config_schema::RuntimeDefaultFloat(kVideoMaxHeight),
		    .frame_width      = config_schema::RuntimeDefaultInt(kVideoFrameWidth),
		    .frame_height     = config_schema::RuntimeDefaultInt(kVideoFrameHeight),
		    .clahe_enabled    = config_schema::RuntimeDefaultBool(kVideoClaheEnabled),
		    .clahe_clip_limit = config_schema::RuntimeDefaultFloat(kVideoClaheClipLimit),
		    .clahe_tile_grid_size = config_schema::RuntimeDefaultInt(kVideoClaheTileGridSize),
		    .dark_threshold       = config_schema::RuntimeDefaultFloat(kVideoDarkThreshold),
		    .force_mjpeg          = config_schema::RuntimeDefaultBool(kVideoForceMjpeg),
		    .exposure             = config_schema::RuntimeDefaultInt(kVideoExposure),
		    .device_fps           = config_schema::RuntimeDefaultInt(kVideoDeviceFps),
		    .rotate               = config_schema::RuntimeDefaultInt(kVideoRotate),
		};
	}

	auto DefaultFaceConfig() -> FaceConfig {
		using enum config_schema::OptionId;
		FaceConfig config{
		    .yunet_score_threshold = config_schema::RuntimeDefaultFloat(kFaceYunetScoreThreshold),
		    .yunet_nms_threshold   = config_schema::RuntimeDefaultFloat(kFaceYunetNmsThreshold),
		    .yunet_top_k           = config_schema::RuntimeDefaultInt(kFaceYunetTopK),
		    .sface_metric          = config_schema::kSfaceDefaultMetric,
		};
		config.sface_threshold = config_schema::RuntimeDefaultFloat(kFaceSfaceThreshold);
		return config;
	}

}  // namespace howdy::native
