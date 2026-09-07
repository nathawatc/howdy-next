#pragma once

#include "vision/face_metric.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

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
		std::string_view                  description;
	};

	constexpr auto BoolDefault(bool value) -> RuntimeDefault {
		return {.has_boolean = true, .boolean = value};
	}

	constexpr auto IntDefault(int value) -> RuntimeDefault {
		return {.has_integer = true, .integer = value};
	}

	constexpr auto FloatDefault(float value) -> RuntimeDefault {
		return {.has_floating_point = true, .floating_point = value};
	}

	constexpr auto StringDefault(std::string_view value) -> RuntimeDefault {
		return {.has_string = true, .string = value};
	}

	inline constexpr FaceMetric kSfaceDefaultMetric = FaceMetric::kCosine;

	auto IsAcceptedBooleanText(std::string_view value) -> bool;
	auto FormatIntegerValue(int value) -> std::optional<std::string>;
	auto FormatFloatingPointValue(float value) -> std::optional<std::string>;
	auto FormatFallbackValue(const Option &option) -> std::optional<std::string>;

	auto ValidateOptions(std::span<const Option> options) -> std::optional<std::string>;
	auto RuntimeConfigOptions() -> std::span<const Option>;
	auto RuntimeConfigOption(OptionId id) -> const Option &;
	auto RuntimeConfigOption(std::string_view section, std::string_view key) -> const Option *;
	auto RuntimeDefaultBool(OptionId id) -> bool;
	auto RuntimeDefaultInt(OptionId id) -> int;
	auto RuntimeDefaultFloat(OptionId id) -> float;
	auto RuntimeDefaultString(OptionId id) -> std::string_view;

}  // namespace howdy::native::config_schema
