#pragma once

#include "support/face_metric.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace howdy::native::config_schema {

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

	using NumericRange = std::variant<std::monostate, IntegerRange, FloatRange>;

	using RuntimeDefault = std::variant<std::monostate, bool, int, float, std::string_view>;

	// A typed option value as written to config.toml.
	using ConfigValue = std::variant<bool, int, float, std::string>;

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

	// The only boolean spellings config.toml, `howdy set`, and `howdy disable` accept.
	inline constexpr std::array<std::string_view, 2> kBooleanValues{"false", "true"};

	// The option's "section.key" name.
	auto OptionName(const Option &option) -> std::string;

	auto FallbackConfigValue(const Option &option) -> std::optional<ConfigValue>;
	auto FormatIntegerValue(int value) -> std::optional<std::string>;
	auto FormatFloatingPointValue(float value) -> std::optional<std::string>;
	// Canonical text of a typed value, as diagnostics and documentation show it.
	auto FormatConfigValue(const ConfigValue &value) -> std::string;

	auto ValidateOptions(std::span<const Option> options) -> std::optional<std::string>;
	auto RuntimeConfigOptions() -> std::span<const Option>;
	auto RuntimeConfigOption(OptionId id) -> const Option &;
	auto RuntimeConfigOption(std::string_view section, std::string_view key) -> const Option *;
	// Option keys are unique across sections (ValidateOptions checks it), so `howdy set` names an
	// option by key alone.
	auto RuntimeConfigOptionByKey(std::string_view key) -> const Option *;
	auto IsRuntimeConfigSection(std::string_view section) -> bool;
	auto RuntimeDefaultBool(OptionId id) -> bool;
	auto RuntimeDefaultInt(OptionId id) -> int;
	auto RuntimeDefaultFloat(OptionId id) -> float;
	auto RuntimeDefaultString(OptionId id) -> std::string_view;

}  // namespace howdy::native::config_schema
