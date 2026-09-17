#include "config/config_schema.hpp"

#include "config_schema/internal.hpp"
#include "support/capture_device_path.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <variant>

namespace howdy::native::config_schema {
	namespace {

		inline constexpr NumericRange kTimeoutRange = IntegerRange{.minimum = 1, .maximum = 300};
		inline constexpr NumericRange kMaxHeightRange =
		    FloatRange{.minimum = 32.0F, .maximum = 4096.0F};
		inline constexpr NumericRange kRotateRange = IntegerRange{.minimum = 0, .maximum = 2};
		inline constexpr NumericRange kDarkThresholdRange =
		    FloatRange{.minimum = 0.0F, .maximum = 99.9F};
		inline constexpr NumericRange kClaheClipLimitRange =
		    FloatRange{.minimum = 0.01F, .maximum = 100.0F};
		inline constexpr NumericRange kClaheTileGridSizeRange =
		    IntegerRange{.minimum = 1, .maximum = 64};
		inline constexpr NumericRange kYunetScoreThresholdRange =
		    FloatRange{.minimum = 0.0F, .maximum = 1.0F};
		inline constexpr NumericRange kYunetNmsThresholdRange =
		    FloatRange{.minimum = 0.0F, .maximum = 1.0F};
		inline constexpr NumericRange kYunetTopKRange =
		    IntegerRange{.minimum = 1, .maximum = 10000};
		inline constexpr NumericRange kFrameSizeRange = IntegerRange{
		    .minimum       = 16,
		    .maximum       = 8192,
		    .allowed_value = -1,
		};
		inline constexpr NumericRange kDeviceFpsRange = IntegerRange{.minimum = 0, .maximum = 480};
		inline constexpr NumericRange kExposureRange  = IntegerRange{
		    .minimum       = 0,
		    .maximum       = 10000,
		    .allowed_value = -1,
		};
		inline constexpr NumericRange kSfaceThresholdRange = FloatRange{
		    .minimum = 0.0F,
		    .maximum = FaceMetricThresholdMaximum(),
		};
		inline constexpr auto kExpectedBooleanRule = "expected a boolean";
		inline constexpr auto kFrameSizeRule       = "expected -1 or integer range 16..8192";

		inline constexpr auto                            kSfaceMetricChoices = kFaceMetricSpellings;
		inline constexpr std::array<std::string_view, 1> kDevicePathChoices  = {kNoCaptureDevice};

		inline constexpr auto kRuntimeConfigOptions = std::to_array<Option>({
		    Option{.id           = OptionId::kCoreDetectionNotice,
		           .section      = "core",
		           .key          = "detection_notice",
		           .type         = ValueType::kBoolean,
		           .fallback     = BoolDefault(false),
		           .invalid_rule = kExpectedBooleanRule,
		           .description  = "Show progress messages for each face scan attempt."},
		    Option{.id           = OptionId::kCoreNoConfirmation,
		           .section      = "core",
		           .key          = "no_confirmation",
		           .type         = ValueType::kBoolean,
		           .fallback     = BoolDefault(false),
		           .invalid_rule = kExpectedBooleanRule,
		           .description  = "Suppress success confirmation after facial authentication."},
		    Option{.id           = OptionId::kCoreAbortIfSsh,
		           .section      = "core",
		           .key          = "abort_if_ssh",
		           .type         = ValueType::kBoolean,
		           .fallback     = BoolDefault(true),
		           .invalid_rule = kExpectedBooleanRule,
		           .description  = "Skip facial authentication when session uses SSH."},
		    Option{.id           = OptionId::kCoreAbortIfLidClosed,
		           .section      = "core",
		           .key          = "abort_if_lid_closed",
		           .type         = ValueType::kBoolean,
		           .fallback     = BoolDefault(true),
		           .invalid_rule = kExpectedBooleanRule,
		           .description  = "Skip facial authentication while laptop lid is closed."},
		    Option{.id           = OptionId::kCoreDisabled,
		           .section      = "core",
		           .key          = "disabled",
		           .type         = ValueType::kBoolean,
		           .fallback     = BoolDefault(false),
		           .invalid_rule = kExpectedBooleanRule,
		           .description =
		               "Disable Howdy PAM authentication without removing enrolled models."},
		    Option{.id           = OptionId::kVideoTimeout,
		           .section      = "video",
		           .key          = "timeout",
		           .type         = ValueType::kInteger,
		           .fallback     = IntDefault(4),
		           .range        = kTimeoutRange,
		           .invalid_rule = "expected integer range 1..300",
		           .description  = "Maximum seconds allowed for one facial authentication scan."},
		    Option{.id           = OptionId::kVideoDevicePath,
		           .section      = "video",
		           .key          = "device_path",
		           .type         = ValueType::kString,
		           .fallback     = StringDefault(kNoCaptureDevice),
		           .choices      = kDevicePathChoices,
		           .special_rule = SpecialRule::kDevicePath,
		           .invalid_rule =
		               "expected none, /dev/video*, /dev/v4l/by-path/*, or /dev/v4l/by-id/*",
		           .description =
		               "Camera device path; prefer a stable /dev/v4l/by-path or /dev/v4l/by-id "
		               "entry."},
		    Option{.id           = OptionId::kVideoWarnNoDevice,
		           .section      = "video",
		           .key          = "warn_no_device",
		           .type         = ValueType::kBoolean,
		           .fallback     = BoolDefault(true),
		           .invalid_rule = kExpectedBooleanRule,
		           .description  = "Show warning when configured camera cannot be opened."},
		    Option{.id           = OptionId::kVideoMaxHeight,
		           .section      = "video",
		           .key          = "max_height",
		           .type         = ValueType::kFloatingPoint,
		           .fallback     = FloatDefault(320.0F),
		           .range        = kMaxHeightRange,
		           .invalid_rule = "expected range 32..4096",
		           .description  = "Limit processed frame height to trade image detail for speed."},
		    Option{.id           = OptionId::kVideoFrameWidth,
		           .section      = "video",
		           .key          = "frame_width",
		           .type         = ValueType::kInteger,
		           .fallback     = IntDefault(-1),
		           .range        = kFrameSizeRange,
		           .invalid_rule = kFrameSizeRule,
		           .description = "Requested camera width; -1 selects default or largest profile."},
		    Option{.id           = OptionId::kVideoFrameHeight,
		           .section      = "video",
		           .key          = "frame_height",
		           .type         = ValueType::kInteger,
		           .fallback     = IntDefault(-1),
		           .range        = kFrameSizeRange,
		           .invalid_rule = kFrameSizeRule,
		           .description =
		               "Requested camera height; -1 selects default or largest profile."},
		    Option{.id           = OptionId::kVideoClaheEnabled,
		           .section      = "video",
		           .key          = "clahe_enabled",
		           .type         = ValueType::kBoolean,
		           .fallback     = BoolDefault(true),
		           .invalid_rule = kExpectedBooleanRule,
		           .description  = "Enable CLAHE preprocessing for low-contrast camera frames."},
		    Option{.id           = OptionId::kVideoClaheClipLimit,
		           .section      = "video",
		           .key          = "clahe_clip_limit",
		           .type         = ValueType::kFloatingPoint,
		           .fallback     = FloatDefault(1.25F),
		           .range        = kClaheClipLimitRange,
		           .invalid_rule = "expected range 0.01..100",
		           .description  = "CLAHE contrast clip limit for low-contrast frames."},
		    Option{.id           = OptionId::kVideoClaheTileGridSize,
		           .section      = "video",
		           .key          = "clahe_tile_grid_size",
		           .type         = ValueType::kInteger,
		           .fallback     = IntDefault(8),
		           .range        = kClaheTileGridSizeRange,
		           .invalid_rule = "expected integer range 1..64",
		           .description  = "CLAHE grid size used to process each camera frame."},
		    Option{.id           = OptionId::kVideoDarkThreshold,
		           .section      = "video",
		           .key          = "dark_threshold",
		           .type         = ValueType::kFloatingPoint,
		           .fallback     = FloatDefault(75.0F),
		           .range        = kDarkThresholdRange,
		           .invalid_rule = "expected range 0..99.9",
		           .description =
		               "Reject frames whose darkest histogram bin exceeds this percentage."},
		    Option{.id           = OptionId::kVideoForceMjpeg,
		           .section      = "video",
		           .key          = "force_mjpeg",
		           .type         = ValueType::kBoolean,
		           .fallback     = BoolDefault(false),
		           .invalid_rule = kExpectedBooleanRule,
		           .description  = "Force OpenCV to decode camera frames as MJPEG."},
		    Option{.id           = OptionId::kVideoExposure,
		           .section      = "video",
		           .key          = "exposure",
		           .type         = ValueType::kInteger,
		           .fallback     = IntDefault(-1),
		           .range        = kExposureRange,
		           .invalid_rule = "expected -1 or integer range 0..10000",
		           .description  = "Camera exposure value; -1 leaves automatic exposure enabled."},
		    Option{.id           = OptionId::kVideoDeviceFps,
		           .section      = "video",
		           .key          = "device_fps",
		           .type         = ValueType::kInteger,
		           .fallback     = IntDefault(0),
		           .range        = kDeviceFpsRange,
		           .invalid_rule = "expected integer range 0..480",
		           .description  = "Camera frame rate; 0 leaves selection to camera backend."},
		    Option{.id           = OptionId::kVideoRotate,
		           .section      = "video",
		           .key          = "rotate",
		           .type         = ValueType::kInteger,
		           .fallback     = IntDefault(0),
		           .range        = kRotateRange,
		           .invalid_rule = "expected integer range 0..2",
		           .description  = "Camera orientation mode: 0 landscape, 1 both, 2 portrait."},
		    Option{.id           = OptionId::kFaceYunetScoreThreshold,
		           .section      = "face",
		           .key          = "yunet_score_threshold",
		           .type         = ValueType::kFloatingPoint,
		           .fallback     = FloatDefault(0.8845F),
		           .range        = kYunetScoreThresholdRange,
		           .invalid_rule = "expected range 0..1",
		           .description  = "YuNet face detector score threshold."},
		    Option{.id           = OptionId::kFaceYunetNmsThreshold,
		           .section      = "face",
		           .key          = "yunet_nms_threshold",
		           .type         = ValueType::kFloatingPoint,
		           .fallback     = FloatDefault(0.3F),
		           .range        = kYunetNmsThresholdRange,
		           .invalid_rule = "expected range 0..1",
		           .description  = "YuNet detector non-maximum suppression threshold."},
		    Option{.id           = OptionId::kFaceYunetTopK,
		           .section      = "face",
		           .key          = "yunet_top_k",
		           .type         = ValueType::kInteger,
		           .fallback     = IntDefault(1000),
		           .range        = kYunetTopKRange,
		           .invalid_rule = "expected integer range 1..10000",
		           .description  = "Maximum number of YuNet detections retained for matching."},
		    Option{.id           = OptionId::kFaceSfaceMetric,
		           .section      = "face",
		           .key          = "sface_metric",
		           .type         = ValueType::kString,
		           .fallback     = StringDefault(FaceMetricSpelling(kSfaceDefaultMetric)),
		           .choices      = kSfaceMetricChoices,
		           .invalid_rule = "expected one of: cosine, l2, l2norm",
		           .description  = "SFace distance metric used to compare face embeddings."},
		    Option{.id           = OptionId::kFaceSfaceThreshold,
		           .section      = "face",
		           .key          = "sface_threshold",
		           .type         = ValueType::kFloatingPoint,
		           .fallback     = FloatDefault(0.6942F),
		           .range        = kSfaceThresholdRange,
		           .special_rule = SpecialRule::kSfaceThreshold,
		           .invalid_rule = "expected a floating-point value",
		           .description = "SFace match threshold; valid range depends on selected metric."},
		    Option{.id           = OptionId::kDebugEndReport,
		           .section      = "debug",
		           .key          = "end_report",
		           .type         = ValueType::kBoolean,
		           .fallback     = BoolDefault(false),
		           .invalid_rule = kExpectedBooleanRule,
		           .description  = "Print timing details after authentication."},
		});

	}  // namespace

	auto ValidateOptions(std::span<const Option> options) -> std::optional<std::string> {
		return config_schema_internal::ValidateSchemaOptions(options);
	}

	auto RuntimeConfigOptions() -> std::span<const Option> {
		return kRuntimeConfigOptions;
	}

	auto RuntimeConfigOption(OptionId id) -> const Option & {
		for (const auto &option : kRuntimeConfigOptions) {
			if (option.id == id) {
				return option;
			}
		}
		std::abort();
	}

	auto RuntimeConfigOption(std::string_view section, std::string_view key) -> const Option * {
		for (const auto &option : kRuntimeConfigOptions) {
			if (option.section == section && option.key == key) {
				return &option;
			}
		}
		return nullptr;
	}

	auto IsAcceptedBooleanText(std::string_view value) -> bool {
		std::string lowered;
		lowered.reserve(value.size());
		for (const char character : value) {
			lowered.push_back(
			    static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
		}
		return std::ranges::find(kAcceptedBooleanSpellings, lowered) !=
		       kAcceptedBooleanSpellings.end();
	}

	auto FormatIntegerValue(int value) -> std::optional<std::string> {
		std::array<char, 32> buffer{};
		const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, 10);
		if (result.ec != std::errc{}) {
			return std::nullopt;
		}
		return std::string(buffer.data(), result.ptr);
	}

	auto FormatFloatingPointValue(float value) -> std::optional<std::string> {
		if (!std::isfinite(value)) {
			return std::nullopt;
		}
		std::array<char, 64> buffer{};
		const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
		                                  std::chars_format::general);
		if (result.ec != std::errc{}) {
			return std::nullopt;
		}
		return std::string(buffer.data(), result.ptr);
	}

	namespace {

		auto FormatRuntimeDefault(const RuntimeDefault &value) -> std::optional<std::string> {
			struct Visitor {
				auto operator()(std::monostate unused) const -> std::optional<std::string> {
					(void)unused;
					return std::nullopt;
				}

				auto operator()(bool val) const -> std::optional<std::string> {
					return val ? "true" : "false";
				}

				auto operator()(int val) const -> std::optional<std::string> {
					return FormatIntegerValue(val);
				}

				auto operator()(float val) const -> std::optional<std::string> {
					return FormatFloatingPointValue(val);
				}

				auto operator()(std::string_view val) const -> std::optional<std::string> {
					return std::string(val);
				}
			};

			return std::visit(Visitor{}, value);
		}

	}  // namespace

	auto FormatFallbackValue(const Option &option) -> std::optional<std::string> {
		return FormatRuntimeDefault(option.fallback);
	}

	auto RuntimeDefaultBool(OptionId id) -> bool {
		const auto &fallback = RuntimeConfigOption(id).fallback;
		const auto *val      = std::get_if<bool>(&fallback);
		if (val == nullptr) {
			std::abort();
		}
		return *val;
	}

	auto RuntimeDefaultInt(OptionId id) -> int {
		const auto &fallback = RuntimeConfigOption(id).fallback;
		const auto *val      = std::get_if<int>(&fallback);
		if (val == nullptr) {
			std::abort();
		}
		return *val;
	}

	auto RuntimeDefaultFloat(OptionId id) -> float {
		const auto &fallback = RuntimeConfigOption(id).fallback;
		const auto *val      = std::get_if<float>(&fallback);
		if (val == nullptr) {
			std::abort();
		}
		return *val;
	}

	auto RuntimeDefaultString(OptionId id) -> std::string_view {
		const auto &fallback = RuntimeConfigOption(id).fallback;
		const auto *val      = std::get_if<std::string_view>(&fallback);
		if (val == nullptr) {
			std::abort();
		}
		return *val;
	}

}  // namespace howdy::native::config_schema
