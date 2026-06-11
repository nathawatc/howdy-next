#include "config/config_schema.hpp"

#include <array>

namespace howdy::native::config_schema {
	namespace {

		inline constexpr NumericRange timeout_range{.minimum = 1.0F, .maximum = 300.0F};
		inline constexpr NumericRange max_height_range{.minimum = 32.0F, .maximum = 4096.0F};
		inline constexpr NumericRange rotate_range{.minimum = 0.0F, .maximum = 2.0F};
		inline constexpr NumericRange dark_threshold_range{.minimum = 0.0F, .maximum = 99.9F};
		inline constexpr NumericRange clahe_clip_limit_range{.minimum = 0.01F, .maximum = 100.0F};
		inline constexpr NumericRange clahe_tile_grid_size_range{.minimum = 1.0F, .maximum = 64.0F};
		inline constexpr NumericRange yunet_score_threshold_range{.minimum = 0.0F, .maximum = 1.0F};
		inline constexpr NumericRange yunet_nms_threshold_range{.minimum = 0.0F, .maximum = 1.0F};
		inline constexpr NumericRange yunet_top_k_range{.minimum = 1.0F, .maximum = 10000.0F};
		inline constexpr NumericRange frame_size_range{
		    .minimum           = 16.0F,
		    .maximum           = 8192.0F,
		    .has_allowed_value = true,
		    .allowed_value     = -1.0F,
		};
		inline constexpr NumericRange device_fps_range{.minimum = 0.0F, .maximum = 480.0F};
		inline constexpr NumericRange exposure_range{
		    .minimum           = 0.0F,
		    .maximum           = 10000.0F,
		    .has_allowed_value = true,
		    .allowed_value     = -1.0F,
		};
		inline constexpr NumericRange sface_threshold_range{.minimum = 0.0F, .maximum = 4.0F};

		inline constexpr std::array<std::string_view, 3> sface_metric_choices = {
		    "cosine",
		    "l2",
		    "l2norm",
		};
		inline constexpr std::array<std::string_view, 1> device_path_choices = {"none"};
		inline constexpr std::array<std::string_view, 2> model_path_choices  = {"default", "none"};

		inline constexpr auto kRuntimeConfigOptions = std::to_array<Option>({
		    Option{.section      = "core",
		           .key          = "detection_notice",
		           .type         = ValueType::boolean,
		           .fallback     = bool_default(true),
		           .invalid_rule = "expected a boolean"},
		    Option{.section      = "core",
		           .key          = "no_confirmation",
		           .type         = ValueType::boolean,
		           .fallback     = bool_default(true),
		           .invalid_rule = "expected a boolean"},
		    Option{.section      = "core",
		           .key          = "abort_if_ssh",
		           .type         = ValueType::boolean,
		           .fallback     = bool_default(true),
		           .invalid_rule = "expected a boolean"},
		    Option{.section      = "core",
		           .key          = "abort_if_lid_closed",
		           .type         = ValueType::boolean,
		           .fallback     = bool_default(true),
		           .invalid_rule = "expected a boolean"},
		    Option{.section      = "core",
		           .key          = "disabled",
		           .type         = ValueType::boolean,
		           .fallback     = bool_default(false),
		           .invalid_rule = "expected a boolean"},
		    Option{.section      = "video",
		           .key          = "timeout",
		           .type         = ValueType::integer,
		           .fallback     = int_default(4),
		           .range        = timeout_range,
		           .invalid_rule = "expected integer range 1..300"},
		    Option{.section      = "video",
		           .key          = "device_path",
		           .type         = ValueType::string,
		           .fallback     = string_default("/dev/video0"),
		           .choices      = device_path_choices,
		           .special_rule = SpecialRule::device_path,
		           .invalid_rule = "expected none, /dev/video*, or /dev/v4l/by-path/*"},
		    Option{.section      = "video",
		           .key          = "warn_no_device",
		           .type         = ValueType::boolean,
		           .fallback     = bool_default(true),
		           .invalid_rule = "expected a boolean"},
		    Option{.section      = "video",
		           .key          = "max_height",
		           .type         = ValueType::floating_point,
		           .fallback     = float_default(320.0F),
		           .range        = max_height_range,
		           .invalid_rule = "expected range 32..4096"},
		    Option{.section      = "video",
		           .key          = "frame_width",
		           .type         = ValueType::integer,
		           .fallback     = int_default(-1),
		           .range        = frame_size_range,
		           .invalid_rule = "expected -1 or integer range 16..8192"},
		    Option{.section      = "video",
		           .key          = "frame_height",
		           .type         = ValueType::integer,
		           .fallback     = int_default(-1),
		           .range        = frame_size_range,
		           .invalid_rule = "expected -1 or integer range 16..8192"},
		    Option{.section      = "video",
		           .key          = "clahe_enabled",
		           .type         = ValueType::boolean,
		           .fallback     = bool_default(true),
		           .invalid_rule = "expected a boolean"},
		    Option{.section      = "video",
		           .key          = "clahe_clip_limit",
		           .type         = ValueType::floating_point,
		           .fallback     = float_default(1.25F),
		           .range        = clahe_clip_limit_range,
		           .invalid_rule = "expected range 0.01..100"},
		    Option{.section      = "video",
		           .key          = "clahe_tile_grid_size",
		           .type         = ValueType::integer,
		           .fallback     = int_default(8),
		           .range        = clahe_tile_grid_size_range,
		           .invalid_rule = "expected integer range 1..64"},
		    Option{.section      = "video",
		           .key          = "dark_threshold",
		           .type         = ValueType::floating_point,
		           .fallback     = float_default(60.0F),
		           .range        = dark_threshold_range,
		           .invalid_rule = "expected range 0..99.9"},
		    Option{.section      = "video",
		           .key          = "force_mjpeg",
		           .type         = ValueType::boolean,
		           .fallback     = bool_default(false),
		           .invalid_rule = "expected a boolean"},
		    Option{.section      = "video",
		           .key          = "exposure",
		           .type         = ValueType::integer,
		           .fallback     = int_default(-1),
		           .range        = exposure_range,
		           .invalid_rule = "expected -1 or integer range 0..10000"},
		    Option{.section      = "video",
		           .key          = "device_fps",
		           .type         = ValueType::integer,
		           .fallback     = int_default(0),
		           .range        = device_fps_range,
		           .invalid_rule = "expected integer range 0..480"},
		    Option{.section      = "video",
		           .key          = "rotate",
		           .type         = ValueType::integer,
		           .fallback     = int_default(0),
		           .range        = rotate_range,
		           .invalid_rule = "expected integer range 0..2"},
		    Option{.section      = "face",
		           .key          = "yunet_model",
		           .type         = ValueType::string,
		           .choices      = model_path_choices,
		           .special_rule = SpecialRule::model_path,
		           .invalid_rule = "expected an absolute path, default, or none"},
		    Option{.section      = "face",
		           .key          = "sface_model",
		           .type         = ValueType::string,
		           .choices      = model_path_choices,
		           .special_rule = SpecialRule::model_path,
		           .invalid_rule = "expected an absolute path, default, or none"},
		    Option{.section      = "face",
		           .key          = "yunet_score_threshold",
		           .type         = ValueType::floating_point,
		           .fallback     = float_default(0.9F),
		           .range        = yunet_score_threshold_range,
		           .invalid_rule = "expected range 0..1"},
		    Option{.section      = "face",
		           .key          = "yunet_nms_threshold",
		           .type         = ValueType::floating_point,
		           .fallback     = float_default(0.3F),
		           .range        = yunet_nms_threshold_range,
		           .invalid_rule = "expected range 0..1"},
		    Option{.section      = "face",
		           .key          = "yunet_top_k",
		           .type         = ValueType::integer,
		           .fallback     = int_default(5000),
		           .range        = yunet_top_k_range,
		           .invalid_rule = "expected integer range 1..10000"},
		    Option{.section      = "face",
		           .key          = "sface_metric",
		           .type         = ValueType::string,
		           .fallback     = string_default("cosine"),
		           .choices      = sface_metric_choices,
		           .invalid_rule = "expected one of: cosine, l2, l2norm"},
		    Option{.section      = "face",
		           .key          = "sface_threshold",
		           .type         = ValueType::floating_point,
		           .fallback     = float_default(sface_cosine_threshold_default),
		           .range        = sface_threshold_range,
		           .special_rule = SpecialRule::sface_threshold,
		           .invalid_rule = "expected a floating-point value"},
		    Option{.section      = "snapshots",
		           .key          = "save_failed",
		           .type         = ValueType::boolean,
		           .invalid_rule = "expected a boolean"},
		    Option{.section      = "snapshots",
		           .key          = "save_successful",
		           .type         = ValueType::boolean,
		           .invalid_rule = "expected a boolean"},
		    Option{.section      = "debug",
		           .key          = "end_report",
		           .type         = ValueType::boolean,
		           .fallback     = bool_default(false),
		           .invalid_rule = "expected a boolean"},
		});

	}  // namespace

	auto runtime_config_options() -> std::span<const Option> {
		return kRuntimeConfigOptions;
	}

	auto runtime_config_option(std::string_view section, std::string_view key) -> const Option * {
		for (const auto &option : kRuntimeConfigOptions) {
			if (option.section == section && option.key == key) {
				return &option;
			}
		}
		return nullptr;
	}

}  // namespace howdy::native::config_schema
