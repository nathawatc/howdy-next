#pragma once

#include "support/face_metric.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace howdy::native::config_schema {

	inline constexpr std::array<std::string_view, 8> kAcceptedBooleanSpellings = {
	    "true", "false", "1", "0", "yes", "no", "on", "off",
	};

	enum class ValueType : std::uint8_t {
		kBoolean,
		kInteger,
		kFloatingPoint,
		kString,
	};

	enum class SpecialRule : std::uint8_t {
		kNone,
		kDevicePath,
		kSfaceThreshold,
	};

	enum class OptionId : std::uint8_t {
		kCoreDetectionNotice,
		kCoreNoConfirmation,
		kCoreAbortIfSsh,
		kCoreAbortIfLidClosed,
		kCoreDisabled,
		kVideoTimeout,
		kVideoDevicePath,
		kVideoWarnNoDevice,
		kVideoMaxHeight,
		kVideoFrameWidth,
		kVideoFrameHeight,
		kVideoClaheEnabled,
		kVideoClaheClipLimit,
		kVideoClaheTileGridSize,
		kVideoDarkThreshold,
		kVideoForceMjpeg,
		kVideoExposure,
		kVideoDeviceFps,
		kVideoRotate,
		kFaceYunetScoreThreshold,
		kFaceYunetNmsThreshold,
		kFaceYunetTopK,
		kFaceSfaceMetric,
		kFaceSfaceThreshold,
		kDebugEndReport,
		kCount,
	};

	struct IntegerRange {
		int                minimum       = 0;
		int                maximum       = 0;
		std::optional<int> allowed_value = std::nullopt;
	};

	struct FloatRange {
		float minimum = 0.0F;
		float maximum = 0.0F;
	};

	[[nodiscard]] constexpr auto RangeContains(const IntegerRange &range, int value) noexcept
	    -> bool {
		if (range.allowed_value.has_value() && value == *range.allowed_value) {
			return true;
		}
		return value >= range.minimum && value <= range.maximum;
	}

	[[nodiscard]] constexpr auto RangeContains(const FloatRange &range, float value) noexcept
	    -> bool {
		return std::isfinite(value) && value >= range.minimum && value <= range.maximum;
	}

	using NumericRange = std::variant<std::monostate, IntegerRange, FloatRange>;

	using RuntimeDefault = std::variant<std::monostate, bool, int, float, std::string_view>;

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
		std::string_view                  description;
	};

	constexpr auto BoolDefault(bool value) -> RuntimeDefault {
		return value;
	}

	constexpr auto IntDefault(int value) -> RuntimeDefault {
		return value;
	}

	constexpr auto FloatDefault(float value) -> RuntimeDefault {
		return value;
	}

	constexpr auto StringDefault(std::string_view value) -> RuntimeDefault {
		return value;
	}

	inline constexpr FaceMetric kSfaceDefaultMetric = FaceMetric::kCosine;

	[[nodiscard]] auto IsAcceptedBooleanText(std::string_view value) -> bool;

	[[nodiscard]] inline auto OptionName(const Option &option) -> std::string {
		return std::string(option.section) + "." + std::string(option.key);
	}

	[[nodiscard]] auto FormatIntegerValue(int value) -> std::optional<std::string>;
	[[nodiscard]] auto FormatFloatingPointValue(float value) -> std::optional<std::string>;
	// Accepted sface_threshold values for one metric, for example "0..4" or "above 0 up to 1".
	[[nodiscard]] auto FormatSfaceThresholdRange(const FaceMetricPolicy &policy) -> std::string;
	[[nodiscard]] auto FormatFallbackValue(const Option &option) -> std::optional<std::string>;

	[[nodiscard]] auto ValidateOptions(std::span<const Option> options)
	    -> std::optional<std::string>;
	[[nodiscard]] auto RuntimeConfigOptions() -> std::span<const Option>;
	[[nodiscard]] auto RuntimeConfigOption(OptionId id) -> const Option &;
	[[nodiscard]] auto RuntimeDefaultBool(OptionId id) -> bool;
	[[nodiscard]] auto RuntimeDefaultInt(OptionId id) -> int;
	[[nodiscard]] auto RuntimeDefaultFloat(OptionId id) -> float;
	[[nodiscard]] auto RuntimeDefaultString(OptionId id) -> std::string_view;

}  // namespace howdy::native::config_schema
