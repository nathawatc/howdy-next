#pragma once

#include "config/runtime_config.hpp"
#include "test_support.hpp"

namespace howdy::test::compare_engine {

	using howdy::test::Expect;
	using howdy::test::ExpectNear;

	inline auto MakeVideoConfig(float dark_threshold = 25.0F, float max_height = 100.0F,
	                            int rotate = 0, bool clahe_enabled = false)
	    -> howdy::native::VideoConfig {
		return {
		    .timeout              = 1,
		    .device_path          = "dummy",
		    .warn_no_device       = false,
		    .max_height           = max_height,
		    .frame_width          = 1,
		    .frame_height         = 1,
		    .clahe_enabled        = clahe_enabled,
		    .clahe_clip_limit     = 2.5F,
		    .clahe_tile_grid_size = 4,
		    .dark_threshold       = dark_threshold,
		    .force_mjpeg          = false,
		    .exposure             = -1,
		    .device_fps           = 30,
		    .rotate               = rotate,
		};
	}

}  // namespace howdy::test::compare_engine

auto RunCompareEngineFrameTests() -> bool;
auto RunCompareEngineInferenceTests() -> bool;
