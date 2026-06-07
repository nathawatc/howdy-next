#pragma once

#include "config/config_reader.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace howdy::native {

    inline auto bounded_int_or_fallback(int value, int fallback, int minimum, int maximum) -> int {
        return value >= minimum && value <= maximum ? value : fallback;
    }

    inline auto bounded_float_or_fallback(float value, float fallback, float minimum, float maximum)
        -> float {
        return value >= minimum && value <= maximum ? value : fallback;
    }

    inline auto config_timeout_seconds(const ConfigReader &config) -> int {
        return bounded_int_or_fallback(config.get_int("video", "timeout", 4), 4, 1, 300);
    }

    inline auto config_dark_threshold(const ConfigReader &config, float fallback = 60.0F) -> float {
        return bounded_float_or_fallback(config.get_float("video", "dark_threshold", fallback),
                                         fallback, 0.0F, 99.9F);
    }

    inline auto config_max_height(const ConfigReader &config) -> float {
        return bounded_float_or_fallback(config.get_float("video", "max_height", 320.0F), 320.0F,
                                         32.0F, 4096.0F);
    }

    inline auto config_rotate_mode(const ConfigReader &config) -> int {
        return bounded_int_or_fallback(config.get_int("video", "rotate", 0), 0, 0, 2);
    }

    inline auto config_exposure(const ConfigReader &config) -> int {
        const int value = config.get_int("video", "exposure", -1);
        if (value == -1) {
            return value;
        }
        return bounded_int_or_fallback(value, -1, 0, 10000);
    }

    inline auto config_clahe_clip_limit(const ConfigReader &config) -> float {
        return bounded_float_or_fallback(config.get_float("video", "clahe_clip_limit", 1.25F),
                                         1.25F, 0.01F, 100.0F);
    }

    inline auto config_clahe_tile_grid_size(const ConfigReader &config) -> int {
        return bounded_int_or_fallback(config.get_int("video", "clahe_tile_grid_size", 8), 8, 1,
                                       64);
    }

    inline auto config_frame_width(const ConfigReader &config) -> int {
        const int value = config.get_int("video", "frame_width", -1);
        if (value == -1) {
            return value;
        }
        return bounded_int_or_fallback(value, -1, 16, 8192);
    }

    inline auto config_frame_height(const ConfigReader &config) -> int {
        const int value = config.get_int("video", "frame_height", -1);
        if (value == -1) {
            return value;
        }
        return bounded_int_or_fallback(value, -1, 16, 8192);
    }

    inline auto config_device_fps(const ConfigReader &config) -> int {
        return bounded_int_or_fallback(config.get_int("video", "device_fps", 0), 0, 0, 480);
    }

    inline auto config_yunet_score_threshold(const ConfigReader &config) -> float {
        return bounded_float_or_fallback(config.get_float("face", "yunet_score_threshold", 0.9F),
                                         0.9F, 0.0F, 1.0F);
    }

    inline auto config_yunet_nms_threshold(const ConfigReader &config) -> float {
        return bounded_float_or_fallback(config.get_float("face", "yunet_nms_threshold", 0.3F),
                                         0.3F, 0.0F, 1.0F);
    }

    inline auto config_yunet_top_k(const ConfigReader &config) -> int {
        return bounded_int_or_fallback(config.get_int("face", "yunet_top_k", 5000), 5000, 1, 10000);
    }

    inline auto config_sface_metric(const ConfigReader &config) -> std::string {
        auto metric = config.get("face", "sface_metric", "cosine");
        std::ranges::transform(metric, metric.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (metric == "cosine" || metric == "l2" || metric == "l2norm") {
            return metric;
        }
        return "cosine";
    }

    inline auto config_sface_threshold(const ConfigReader &config, const std::string &metric)
        -> float {
        const float fallback = metric == "cosine" ? 0.363F : 1.128F;
        const float maximum  = metric == "cosine" ? 1.0F : 4.0F;
        const float minimum  = 0.0F;
        return bounded_float_or_fallback(config.get_float("face", "sface_threshold", fallback),
                                         fallback, minimum, maximum);
    }

}  // namespace howdy::native
