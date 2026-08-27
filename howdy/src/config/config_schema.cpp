#include "config/config_schema.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>

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
		    sface_cosine_metric,
		    "l2",
		    "l2norm",
		};
		inline constexpr std::array<std::string_view, 1> device_path_choices = {"none"};

		auto option_name(const Option &option) -> std::string {
			return std::string(option.section) + "." + std::string(option.key);
		}

		auto is_known_value_type(ValueType type) -> bool {
			switch (type) {
				case ValueType::boolean:
				case ValueType::integer:
				case ValueType::floating_point:
				case ValueType::string:
					return true;
			}
			return false;
		}

		auto fallback_matches_type(const Option &option) -> bool {
			const auto &fallback   = option.fallback;
			const auto  flag_count = static_cast<unsigned>(fallback.has_boolean) +
			                         static_cast<unsigned>(fallback.has_integer) +
			                         static_cast<unsigned>(fallback.has_floating_point) +
			                         static_cast<unsigned>(fallback.has_string);
			if (flag_count != 1U) {
				return false;
			}

			switch (option.type) {
				case ValueType::boolean:
					return fallback.has_boolean;
				case ValueType::integer:
					return fallback.has_integer;
				case ValueType::floating_point:
					return fallback.has_floating_point;
				case ValueType::string:
					return fallback.has_string;
			}
			return false;
		}

		auto range_has_metadata(const NumericRange &range) -> bool {
			return range.minimum != 0.0F || range.maximum != 0.0F || range.has_allowed_value;
		}

		auto is_representable_integer(float value) -> bool {
			if (!std::isfinite(value) || std::trunc(value) != value) {
				return false;
			}
			const auto wide_value = static_cast<long double>(value);
			return wide_value >= static_cast<long double>(std::numeric_limits<int>::min()) &&
			       wide_value <= static_cast<long double>(std::numeric_limits<int>::max());
		}

		auto validate_integer_fallback(const Option &option) -> std::optional<std::string> {
			if (!range_has_metadata(option.range)) {
				return std::nullopt;
			}

			const auto &range    = option.range;
			const auto  fallback = option.fallback.integer;
			if (range.has_allowed_value && fallback == static_cast<int>(range.allowed_value)) {
				return std::nullopt;
			}
			if (fallback < static_cast<int>(range.minimum) ||
			    fallback > static_cast<int>(range.maximum)) {
				return "integer fallback is outside range: " + option_name(option);
			}
			return std::nullopt;
		}

		auto validate_floating_point_fallback(const Option &option) -> std::optional<std::string> {
			const auto fallback = option.fallback.floating_point;
			if (!std::isfinite(fallback)) {
				return "floating-point fallback is not finite: " + option_name(option);
			}
			if (!range_has_metadata(option.range)) {
				return std::nullopt;
			}
			if (fallback < option.range.minimum || fallback > option.range.maximum) {
				return "floating-point fallback is outside range: " + option_name(option);
			}
			return std::nullopt;
		}

		auto validate_range(const Option &option) -> std::optional<std::string> {
			const auto name = option_name(option);
			if (option.type != ValueType::integer && option.type != ValueType::floating_point) {
				if (range_has_metadata(option.range)) {
					return "numeric range is incompatible with option type: " + name;
				}
				return std::nullopt;
			}

			const auto &range = option.range;
			if (!std::isfinite(range.minimum) || !std::isfinite(range.maximum) ||
			    range.minimum > range.maximum) {
				return "invalid numeric range: " + name;
			}
			if (option.type == ValueType::integer && (!is_representable_integer(range.minimum) ||
			                                          !is_representable_integer(range.maximum))) {
				return "integer range is not representable: " + name;
			}

			if (range.has_allowed_value &&
			    (option.type == ValueType::integer ? !is_representable_integer(range.allowed_value)
			                                       : !std::isfinite(range.allowed_value))) {
				return "allowed value is not representable: " + name;
			}
			if (option.type == ValueType::integer) {
				return validate_integer_fallback(option);
			}
			return validate_floating_point_fallback(option);
		}

		auto validate_choices(const Option &option) -> std::optional<std::string> {
			if (option.choices.empty()) {
				return std::nullopt;
			}
			const auto name = option_name(option);
			if (option.type != ValueType::string) {
				return "choices are incompatible with option type: " + name;
			}
			for (std::size_t index = 0; index < option.choices.size(); ++index) {
				if (option.choices[index].empty()) {
					return "choice is empty: " + name;
				}
				if (std::ranges::find(option.choices.first(index), option.choices[index]) !=
				    option.choices.first(index).end()) {
					return "duplicate choice: " + name;
				}
			}
			if (option.special_rule != SpecialRule::device_path &&
			    std::ranges::find(option.choices, option.fallback.string) == option.choices.end()) {
				return "fallback is not one of choices: " + name;
			}
			return std::nullopt;
		}

		auto validate_special_rule(const Option &option) -> std::optional<std::string> {
			const auto name = option_name(option);
			switch (option.special_rule) {
				case SpecialRule::none:
					return std::nullopt;
				case SpecialRule::device_path:
					if (option.type == ValueType::string) {
						return std::nullopt;
					}
					return "special rule/type mismatch: " + name;
				case SpecialRule::sface_threshold:
					if (option.type == ValueType::floating_point) {
						return std::nullopt;
					}
					return "special rule/type mismatch: " + name;
			}
			return "unknown special rule: " + name;
		}

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
		           .invalid_rule =
		               "expected none, /dev/video*, /dev/v4l/by-path/*, or /dev/v4l/by-id/*",
		           .description =
		               "Camera device path; prefer a stable /dev/v4l/by-path or /dev/v4l/by-id "
		               "entry."},
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
		    Option{.id           = OptionId::debug_end_report,
		           .section      = "debug",
		           .key          = "end_report",
		           .type         = ValueType::boolean,
		           .fallback     = bool_default(false),
		           .invalid_rule = "expected a boolean",
		           .description  = "Print timing details after authentication."},
		});

		auto validate_special_fallbacks(std::span<const Option> options)
		    -> std::optional<std::string> {
			const Option *metric_option = nullptr;
			for (const auto &option : options) {
				if (option.id == OptionId::face_sface_metric) {
					metric_option = &option;
					break;
				}
			}
			if (metric_option == nullptr || metric_option->fallback.string != sface_cosine_metric) {
				return std::nullopt;
			}

			for (const auto &option : options) {
				if (option.special_rule == SpecialRule::sface_threshold &&
				    option.fallback.floating_point > sface_cosine_threshold_maximum) {
					return "sface threshold fallback exceeds cosine range: " + option_name(option);
				}
			}
			return std::nullopt;
		}

		auto validate_identity(std::span<const Option> options, std::size_t index)
		    -> std::optional<std::string> {
			const auto &option = options[index];
			const auto  name   = option_name(option);
			if (static_cast<std::size_t>(option.id) >= static_cast<std::size_t>(OptionId::count)) {
				return "option has invalid id: " + name;
			}
			if (option.section.empty()) {
				return "invalid or empty section: " + name;
			}
			if (option.key.empty()) {
				return "invalid or empty key: " + name;
			}
			for (const auto &previous : options.first(index)) {
				if (previous.id == option.id) {
					return "duplicate option id: " + name;
				}
				if (previous.section == option.section && previous.key == option.key) {
					return "duplicate section.key: " + name;
				}
				if (previous.key == option.key) {
					return "duplicate option key: " + name;
				}
			}
			return std::nullopt;
		}

	}  // namespace

	auto validate_options(std::span<const Option> options) -> std::optional<std::string> {
		for (std::size_t index = 0; index < options.size(); ++index) {
			const auto &option = options[index];
			const auto  name   = option_name(option);
			if (const auto error = validate_identity(options, index)) {
				return error;
			}
			if (!is_known_value_type(option.type)) {
				return "unknown option type: " + name;
			}
			if (!fallback_matches_type(option)) {
				return "fallback type mismatch: " + name;
			}
			if (const auto error = validate_range(option)) {
				return error;
			}
			if (const auto error = validate_choices(option)) {
				return error;
			}
			if (const auto error = validate_special_rule(option)) {
				return error;
			}
		}
		if (const auto error = validate_special_fallbacks(options)) {
			return error;
		}
		return std::nullopt;
	}

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
