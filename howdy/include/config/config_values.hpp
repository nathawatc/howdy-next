#pragma once

#include "config/config_reader.hpp"
#include "config/config_schema.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>

namespace howdy::native {

	inline auto bounded_int_or_fallback(int value, int fallback, int minimum, int maximum) -> int {
		return value >= minimum && value <= maximum ? value : fallback;
	}

	inline auto bounded_float_or_fallback(float value, float fallback, float minimum, float maximum)
	    -> float {
		return value >= minimum && value <= maximum ? value : fallback;
	}

	inline auto config_option(std::string_view section, std::string_view key)
	    -> const config_schema::Option & {
		const auto *option = config_schema::runtime_config_option(section, key);
		assert(option != nullptr);
		if (option == nullptr) {
			std::abort();
		}
		return *option;
	}

	inline auto config_option_int(const ConfigReader &config, const config_schema::Option &option,
	                              int fallback) -> int {
		return config.get_int(std::string(option.section), std::string(option.key), fallback);
	}

	inline auto config_option_float(const ConfigReader &config, const config_schema::Option &option,
	                                float fallback) -> float {
		return config.get_float(std::string(option.section), std::string(option.key), fallback);
	}

	inline auto config_option_string(const ConfigReader          &config,
	                                 const config_schema::Option &option,
	                                 const std::string           &fallback) -> std::string {
		return config.get(std::string(option.section), std::string(option.key), fallback);
	}

	inline auto config_timeout_seconds(const ConfigReader &config) -> int {
		const auto &option = config_option("video", "timeout");
		return bounded_int_or_fallback(
		    config_option_int(config, option, option.fallback.integer), option.fallback.integer,
		    static_cast<int>(option.range.minimum), static_cast<int>(option.range.maximum));
	}

	inline auto config_dark_threshold(const ConfigReader &config, float fallback = 60.0F) -> float {
		const auto &option = config_option("video", "dark_threshold");
		return bounded_float_or_fallback(config_option_float(config, option, fallback), fallback,
		                                 option.range.minimum, option.range.maximum);
	}

	inline auto config_max_height(const ConfigReader &config) -> float {
		const auto &option = config_option("video", "max_height");
		return bounded_float_or_fallback(
		    config_option_float(config, option, option.fallback.floating_point),
		    option.fallback.floating_point, option.range.minimum, option.range.maximum);
	}

	inline auto config_rotate_mode(const ConfigReader &config) -> int {
		const auto &option = config_option("video", "rotate");
		return bounded_int_or_fallback(
		    config_option_int(config, option, option.fallback.integer), option.fallback.integer,
		    static_cast<int>(option.range.minimum), static_cast<int>(option.range.maximum));
	}

	inline auto config_exposure(const ConfigReader &config) -> int {
		const auto &option = config_option("video", "exposure");
		const int   value  = config_option_int(config, option, option.fallback.integer);
		if (value == -1) {
			return value;
		}
		return bounded_int_or_fallback(value, option.fallback.integer,
		                               static_cast<int>(option.range.minimum),
		                               static_cast<int>(option.range.maximum));
	}

	inline auto config_clahe_clip_limit(const ConfigReader &config) -> float {
		const auto &option = config_option("video", "clahe_clip_limit");
		return bounded_float_or_fallback(
		    config_option_float(config, option, option.fallback.floating_point),
		    option.fallback.floating_point, option.range.minimum, option.range.maximum);
	}

	inline auto config_clahe_tile_grid_size(const ConfigReader &config) -> int {
		const auto &option = config_option("video", "clahe_tile_grid_size");
		return bounded_int_or_fallback(
		    config_option_int(config, option, option.fallback.integer), option.fallback.integer,
		    static_cast<int>(option.range.minimum), static_cast<int>(option.range.maximum));
	}

	inline auto config_frame_width(const ConfigReader &config) -> int {
		const auto &option = config_option("video", "frame_width");
		const int   value  = config_option_int(config, option, option.fallback.integer);
		if (value == -1) {
			return value;
		}
		return bounded_int_or_fallback(value, option.fallback.integer,
		                               static_cast<int>(option.range.minimum),
		                               static_cast<int>(option.range.maximum));
	}

	inline auto config_frame_height(const ConfigReader &config) -> int {
		const auto &option = config_option("video", "frame_height");
		const int   value  = config_option_int(config, option, option.fallback.integer);
		if (value == -1) {
			return value;
		}
		return bounded_int_or_fallback(value, option.fallback.integer,
		                               static_cast<int>(option.range.minimum),
		                               static_cast<int>(option.range.maximum));
	}

	inline auto config_device_fps(const ConfigReader &config) -> int {
		const auto &option = config_option("video", "device_fps");
		return bounded_int_or_fallback(
		    config_option_int(config, option, option.fallback.integer), option.fallback.integer,
		    static_cast<int>(option.range.minimum), static_cast<int>(option.range.maximum));
	}

	inline auto config_yunet_score_threshold(const ConfigReader &config) -> float {
		const auto &option = config_option("face", "yunet_score_threshold");
		return bounded_float_or_fallback(
		    config_option_float(config, option, option.fallback.floating_point),
		    option.fallback.floating_point, option.range.minimum, option.range.maximum);
	}

	inline auto config_yunet_nms_threshold(const ConfigReader &config) -> float {
		const auto &option = config_option("face", "yunet_nms_threshold");
		return bounded_float_or_fallback(
		    config_option_float(config, option, option.fallback.floating_point),
		    option.fallback.floating_point, option.range.minimum, option.range.maximum);
	}

	inline auto config_yunet_top_k(const ConfigReader &config) -> int {
		const auto &option = config_option("face", "yunet_top_k");
		return bounded_int_or_fallback(
		    config_option_int(config, option, option.fallback.integer), option.fallback.integer,
		    static_cast<int>(option.range.minimum), static_cast<int>(option.range.maximum));
	}

	inline auto config_sface_metric(const ConfigReader &config) -> std::string {
		const auto &option = config_option("face", "sface_metric");
		auto metric = config_option_string(config, option, std::string(option.fallback.string));
		std::ranges::transform(metric, metric.begin(), [](unsigned char ch) {
			return static_cast<char>(std::tolower(ch));
		});
		if (std::ranges::find(option.choices, metric) != option.choices.end()) {
			return metric;
		}
		return std::string(option.fallback.string);
	}

	inline auto config_sface_threshold(const ConfigReader &config, const std::string &metric)
	    -> float {
		const auto &option   = config_option("face", "sface_threshold");
		const float fallback = metric == "cosine" ? config_schema::sface_cosine_threshold_default
		                                          : config_schema::sface_other_threshold_default;
		const float maximum  = metric == "cosine" ? config_schema::sface_cosine_threshold_maximum
		                                          : option.range.maximum;
		return bounded_float_or_fallback(config_option_float(config, option, fallback), fallback,
		                                 option.range.minimum, maximum);
	}

}  // namespace howdy::native
