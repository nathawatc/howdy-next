#pragma once

#include "config/config_reader.hpp"
#include "config/config_schema.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace howdy::native {

	inline auto RuntimeOption(config_schema::OptionId id, config_schema::ValueType type)
	    -> const config_schema::Option & {
		const auto &option = config_schema::RuntimeConfigOption(id);
		if (option.type != type) {
			std::abort();
		}
		return option;
	}

	inline auto ReadRuntimeBool(const ConfigReader &reader, config_schema::OptionId id) -> bool {
		const auto &option = RuntimeOption(id, config_schema::ValueType::kBoolean);
		if (!option.fallback.has_boolean) {
			std::abort();
		}
		return reader.GetBool(std::string(option.section), std::string(option.key),
		                      option.fallback.boolean);
	}

	inline auto ReadRuntimeInt(const ConfigReader &reader, config_schema::OptionId id) -> int {
		const auto &option = RuntimeOption(id, config_schema::ValueType::kInteger);
		if (!option.fallback.has_integer) {
			std::abort();
		}
		const int value = reader.GetInt(std::string(option.section), std::string(option.key),
		                                option.fallback.integer);
		if (option.range.has_allowed_value &&
		    value == static_cast<int>(option.range.allowed_value)) {
			return value;
		}
		return value >= static_cast<int>(option.range.minimum) &&
		               value <= static_cast<int>(option.range.maximum)
		           ? value
		           : option.fallback.integer;
	}

	inline auto ReadRuntimeFloat(const ConfigReader &reader, config_schema::OptionId id) -> float {
		const auto &option = RuntimeOption(id, config_schema::ValueType::kFloatingPoint);
		if (!option.fallback.has_floating_point) {
			std::abort();
		}
		const float value = reader.GetFloat(std::string(option.section), std::string(option.key),
		                                    option.fallback.floating_point);
		return value >= option.range.minimum && value <= option.range.maximum
		           ? value
		           : option.fallback.floating_point;
	}

	inline auto ReadRuntimeString(const ConfigReader &reader, config_schema::OptionId id)
	    -> std::string {
		const auto &option = RuntimeOption(id, config_schema::ValueType::kString);
		if (!option.fallback.has_string) {
			std::abort();
		}
		auto value = reader.Get(std::string(option.section), std::string(option.key),
		                        std::string(option.fallback.string));
		if (option.choices.empty() ||
		    option.special_rule == config_schema::SpecialRule::kDevicePath) {
			return value;
		}

		std::ranges::transform(value, value.begin(), [](unsigned char ch) -> char {
			return static_cast<char>(std::tolower(ch));
		});
		return std::ranges::find(option.choices, value) != option.choices.end()
		           ? value
		           : std::string(option.fallback.string);
	}

	inline auto ReadSfaceMetric(const ConfigReader &reader) -> std::optional<FaceMetric> {
		const auto &option = RuntimeOption(config_schema::OptionId::kFaceSfaceMetric,
		                                   config_schema::ValueType::kString);
		if (!option.fallback.has_string) {
			std::abort();
		}
		const auto value = reader.Get(std::string(option.section), std::string(option.key),
		                              std::string(option.fallback.string));
		return ParseFaceMetric(value.empty() ? option.fallback.string : value);
	}

	inline auto ReadSfaceThreshold(const ConfigReader &reader, FaceMetric metric) -> float {
		const auto &option = RuntimeOption(config_schema::OptionId::kFaceSfaceThreshold,
		                                   config_schema::ValueType::kFloatingPoint);
		const auto *policy = GetFaceMetricPolicy(metric);
		if (option.special_rule != config_schema::SpecialRule::kSfaceThreshold ||
		    !option.fallback.has_floating_point || policy == nullptr) {
			std::abort();
		}
		const float value = reader.GetFloat(std::string(option.section), std::string(option.key),
		                                    option.fallback.floating_point);
		return value >= option.range.minimum && value <= policy->threshold_maximum
		           ? value
		           : option.fallback.floating_point;
	}

}  // namespace howdy::native
