#include "config/config_schema.hpp"

#include <array>
#include <cstdlib>

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

		inline constexpr auto kRuntimeConfigOptions = std::to_array<Option>({
		    Option{.id           = OptionId::core_detection_notice,
		           .section      = "core",
		           .key          = "detection_notice",
		           .type         = ValueType::boolean,
		           .fallback     = bool_default(false),
		           .invalid_rule = "expected a boolean",
		           .description  = "Show progress messages for each face scan attempt."},
		    Option{.id           = OptionId::core_no_confirmation,
		           .section      = "core",
		           .key          = "no_confirmation",
		           .type         = ValueType::boolean,
		           .fallback     = bool_default(false),
		           .invalid_rule = "expected a boolean",
		           .description  = "Suppress success confirmation after facial authentication."},
		    Option{.id           = OptionId::core_abort_if_ssh,
		           .section      = "core",
		           .key          = "abort_if_ssh",
		           .type         = ValueType::boolean,
		           .fallback     = bool_default(true),
		           .invalid_rule = "expected a boolean",
		           .description  = "Skip facial authentication when session uses SSH."},
		    Option{.id           = OptionId::core_abort_if_lid_closed,
		           .section      = "core",
		           .key          = "abort_if_lid_closed",
		           .type         = ValueType::boolean,
		           .fallback     = bool_default(true),
		           .invalid_rule = "expected a boolean",
		           .description  = "Skip facial authentication while laptop lid is closed."},
		    Option{.id           = OptionId::core_disabled,
		           .section      = "core",
		           .key          = "disabled",
		           .type         = ValueType::boolean,
		           .fallback     = bool_default(false),
		           .invalid_rule = "expected a boolean",
		           .description =
		               "Disable Howdy PAM authentication without removing enrolled models."},
		    Option{.id           = OptionId::video_timeout,
		           .section      = "video",
		           .key          = "timeout",
		           .type         = ValueType::integer,
		           .fallback     = int_default(4),
		           .range        = timeout_range,
		           .invalid_rule = "expected integer range 1..300",
		           .description  = "Maximum seconds allowed for one facial authentication scan."},
		    Option{.id           = OptionId::video_device_path,
		           .section      = "video",
		           .key          = "device_path",
		           .type         = ValueType::string,
		           .fallback     = string_default("none"),
		           .choices      = device_path_choices,
		           .special_rule = SpecialRule::device_path,
		           .invalid_rule = "expected none, /dev/video*, or /dev/v4l/by-path/*",
		           .description  = "Camera device path; prefer a stable /dev/v4l/by-path entry."},
		    Option{.id           = OptionId::video_warn_no_device,
		           .section      = "video",
		           .key          = "warn_no_device",
		           .type         = ValueType::boolean,
		           .fallback     = bool_default(true),
		           .invalid_rule = "expected a boolean",
		           .description  = "Show warning when configured camera cannot be opened."},
		    Option{.id           = OptionId::video_max_height,
		           .section      = "video",
		           .key          = "max_height",
		           .type         = ValueType::floating_point,
		           .fallback     = float_default(320.0F),
		           .range        = max_height_range,
		           .invalid_rule = "expected range 32..4096",
		           .description  = "Limit processed frame height to trade image detail for speed."},
		    Option{.id           = OptionId::video_frame_width,
		           .section      = "video",
		           .key          = "frame_width",
		           .type         = ValueType::integer,
		           .fallback     = int_default(-1),
		           .range        = frame_size_range,
		           .invalid_rule = "expected -1 or integer range 16..8192",
		           .description = "Requested camera width; -1 selects default or largest profile."},
		    Option{.id           = OptionId::video_frame_height,
		           .section      = "video",
		           .key          = "frame_height",
		           .type         = ValueType::integer,
		           .fallback     = int_default(-1),
		           .range        = frame_size_range,
		           .invalid_rule = "expected -1 or integer range 16..8192",
		           .description =
		               "Requested camera height; -1 selects default or largest profile."},
		    Option{.id           = OptionId::video_clahe_enabled,
		           .section      = "video",
		           .key          = "clahe_enabled",
		           .type         = ValueType::boolean,
		           .fallback     = bool_default(true),
		           .invalid_rule = "expected a boolean",
		           .description  = "Enable CLAHE preprocessing for low-contrast camera frames."},
		    Option{.id           = OptionId::video_clahe_clip_limit,
		           .section      = "video",
		           .key          = "clahe_clip_limit",
		           .type         = ValueType::floating_point,
		           .fallback     = float_default(1.25F),
		           .range        = clahe_clip_limit_range,
		           .invalid_rule = "expected range 0.01..100",
		           .description  = "CLAHE contrast clip limit for low-contrast frames."},
		    Option{.id           = OptionId::video_clahe_tile_grid_size,
		           .section      = "video",
		           .key          = "clahe_tile_grid_size",
		           .type         = ValueType::integer,
		           .fallback     = int_default(8),
		           .range        = clahe_tile_grid_size_range,
		           .invalid_rule = "expected integer range 1..64",
		           .description  = "CLAHE grid size used to process each camera frame."},
		    Option{.id           = OptionId::video_dark_threshold,
		           .section      = "video",
		           .key          = "dark_threshold",
		           .type         = ValueType::floating_point,
		           .fallback     = float_default(75.0F),
		           .range        = dark_threshold_range,
		           .invalid_rule = "expected range 0..99.9",
		           .description =
		               "Reject frames whose darkest histogram bin exceeds this percentage."},
		    Option{.id           = OptionId::video_force_mjpeg,
		           .section      = "video",
		           .key          = "force_mjpeg",
		           .type         = ValueType::boolean,
		           .fallback     = bool_default(false),
		           .invalid_rule = "expected a boolean",
		           .description  = "Force OpenCV to decode camera frames as MJPEG."},
		    Option{.id           = OptionId::video_exposure,
		           .section      = "video",
		           .key          = "exposure",
		           .type         = ValueType::integer,
		           .fallback     = int_default(-1),
		           .range        = exposure_range,
		           .invalid_rule = "expected -1 or integer range 0..10000",
		           .description  = "Camera exposure value; -1 leaves automatic exposure enabled."},
		    Option{.id           = OptionId::video_device_fps,
		           .section      = "video",
		           .key          = "device_fps",
		           .type         = ValueType::integer,
		           .fallback     = int_default(0),
		           .range        = device_fps_range,
		           .invalid_rule = "expected integer range 0..480",
		           .description  = "Camera frame rate; 0 leaves selection to camera backend."},
		    Option{.id           = OptionId::video_rotate,
		           .section      = "video",
		           .key          = "rotate",
		           .type         = ValueType::integer,
		           .fallback     = int_default(0),
		           .range        = rotate_range,
		           .invalid_rule = "expected integer range 0..2",
		           .description  = "Camera orientation mode: 0 landscape, 1 both, 2 portrait."},
		    Option{.id           = OptionId::face_yunet_score_threshold,
		           .section      = "face",
		           .key          = "yunet_score_threshold",
		           .type         = ValueType::floating_point,
		           .fallback     = float_default(0.8845F),
		           .range        = yunet_score_threshold_range,
		           .invalid_rule = "expected range 0..1",
		           .description  = "YuNet face detector score threshold."},
		    Option{.id           = OptionId::face_yunet_nms_threshold,
		           .section      = "face",
		           .key          = "yunet_nms_threshold",
		           .type         = ValueType::floating_point,
		           .fallback     = float_default(0.3F),
		           .range        = yunet_nms_threshold_range,
		           .invalid_rule = "expected range 0..1",
		           .description  = "YuNet detector non-maximum suppression threshold."},
		    Option{.id           = OptionId::face_yunet_top_k,
		           .section      = "face",
		           .key          = "yunet_top_k",
		           .type         = ValueType::integer,
		           .fallback     = int_default(1000),
		           .range        = yunet_top_k_range,
		           .invalid_rule = "expected integer range 1..10000",
		           .description  = "Maximum number of YuNet detections retained for matching."},
		    Option{.id           = OptionId::face_sface_metric,
		           .section      = "face",
		           .key          = "sface_metric",
		           .type         = ValueType::string,
		           .fallback     = string_default("cosine"),
		           .choices      = sface_metric_choices,
		           .invalid_rule = "expected one of: cosine, l2, l2norm",
		           .description  = "SFace distance metric used to compare face embeddings."},
		    Option{.id           = OptionId::face_sface_threshold,
		           .section      = "face",
		           .key          = "sface_threshold",
		           .type         = ValueType::floating_point,
		           .fallback     = float_default(0.6942F),
		           .range        = sface_threshold_range,
		           .special_rule = SpecialRule::sface_threshold,
		           .invalid_rule = "expected a floating-point value",
		           .description = "SFace match threshold; valid range depends on selected metric."},
		    Option{.id           = OptionId::snapshots_save_failed,
		           .section      = "snapshots",
		           .key          = "save_failed",
		           .type         = ValueType::boolean,
		           .fallback     = bool_default(false),
		           .invalid_rule = "expected a boolean",
		           .description  = "Save snapshots from unsuccessful authentication attempts."},
		    Option{.id           = OptionId::snapshots_save_successful,
		           .section      = "snapshots",
		           .key          = "save_successful",
		           .type         = ValueType::boolean,
		           .fallback     = bool_default(false),
		           .invalid_rule = "expected a boolean",
		           .description  = "Save snapshots from successful authentication attempts."},
		    Option{.id           = OptionId::debug_end_report,
		           .section      = "debug",
		           .key          = "end_report",
		           .type         = ValueType::boolean,
		           .fallback     = bool_default(false),
		           .invalid_rule = "expected a boolean",
		           .description  = "Print timing details after authentication."},
		});

	}  // namespace

	auto runtime_config_options() -> std::span<const Option> {
		return kRuntimeConfigOptions;
	}

	auto runtime_config_option(OptionId id) -> const Option & {
		for (const auto &option : kRuntimeConfigOptions) {
			if (option.id == id) {
				return option;
			}
		}
		std::abort();
	}

	auto runtime_config_option(std::string_view section, std::string_view key) -> const Option * {
		for (const auto &option : kRuntimeConfigOptions) {
			if (option.section == section && option.key == key) {
				return &option;
			}
		}
		return nullptr;
	}

	auto runtime_default_bool(OptionId id) -> bool {
		const auto &fallback = runtime_config_option(id).fallback;
		if (!fallback.has_boolean) {
			std::abort();
		}
		return fallback.boolean;
	}

	auto runtime_default_int(OptionId id) -> int {
		const auto &fallback = runtime_config_option(id).fallback;
		if (!fallback.has_integer) {
			std::abort();
		}
		return fallback.integer;
	}

	auto runtime_default_float(OptionId id) -> float {
		const auto &fallback = runtime_config_option(id).fallback;
		if (!fallback.has_floating_point) {
			std::abort();
		}
		return fallback.floating_point;
	}

	auto runtime_default_string(OptionId id) -> std::string_view {
		const auto &fallback = runtime_config_option(id).fallback;
		if (!fallback.has_string) {
			std::abort();
		}
		return fallback.string;
	}

}  // namespace howdy::native::config_schema
