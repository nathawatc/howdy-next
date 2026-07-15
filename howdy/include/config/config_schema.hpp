#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace howdy::native::config_schema {

	enum class ValueType : std::uint8_t {
		boolean,
		integer,
		floating_point,
		string,
	};

	enum class SpecialRule : std::uint8_t {
		none,
		device_path,
		sface_threshold,
	};

	enum class OptionId : std::uint8_t {
		core_detection_notice,
		core_no_confirmation,
		core_abort_if_ssh,
		core_abort_if_lid_closed,
		core_disabled,
		video_timeout,
		video_device_path,
		video_warn_no_device,
		video_max_height,
		video_frame_width,
		video_frame_height,
		video_clahe_enabled,
		video_clahe_clip_limit,
		video_clahe_tile_grid_size,
		video_dark_threshold,
		video_force_mjpeg,
		video_exposure,
		video_device_fps,
		video_rotate,
		face_yunet_score_threshold,
		face_yunet_nms_threshold,
		face_yunet_top_k,
		face_sface_metric,
		face_sface_threshold,
		snapshots_save_failed,
		snapshots_save_successful,
		debug_end_report,
		count,
	};

	struct NumericRange {
		float minimum;
		float maximum;
		bool  has_allowed_value = false;
		float allowed_value     = 0.0F;
	};

	struct RuntimeDefault {
		bool             has_boolean        = false;
		bool             boolean            = false;
		bool             has_integer        = false;
		int              integer            = 0;
		bool             has_floating_point = false;
		float            floating_point     = 0.0F;
		bool             has_string         = false;
		std::string_view string;
	};

	struct Option {
		OptionId                          id;
		std::string_view                  section;
		std::string_view                  key;
		ValueType                         type;
		RuntimeDefault                    fallback;
		NumericRange                      range;
		std::span<const std::string_view> choices;
		SpecialRule                       special_rule;
		std::string_view                  invalid_rule;
	};

	constexpr auto bool_default(bool value) -> RuntimeDefault {
		return {.has_boolean = true, .boolean = value};
	}

	constexpr auto int_default(int value) -> RuntimeDefault {
		return {.has_integer = true, .integer = value};
	}

	constexpr auto float_default(float value) -> RuntimeDefault {
		return {.has_floating_point = true, .floating_point = value};
	}

	constexpr auto string_default(std::string_view value) -> RuntimeDefault {
		return {.has_string = true, .string = value};
	}

	inline constexpr float sface_cosine_threshold_default = 0.363F;
	inline constexpr float sface_other_threshold_default  = 1.128F;
	inline constexpr float sface_cosine_threshold_maximum = 1.0F;

	auto runtime_config_options() -> std::span<const Option>;
	auto runtime_config_option(OptionId id) -> const Option &;
	auto runtime_config_option(std::string_view section, std::string_view key) -> const Option *;
	auto runtime_default_bool(OptionId id) -> bool;
	auto runtime_default_int(OptionId id) -> int;
	auto runtime_default_float(OptionId id) -> float;
	auto runtime_default_string(OptionId id) -> std::string_view;

}  // namespace howdy::native::config_schema
