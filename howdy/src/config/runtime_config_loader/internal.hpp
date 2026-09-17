#pragma once

#include "../config_reader/internal.hpp"
#include "config/config_schema.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

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
		const auto &option   = RuntimeOption(id, config_schema::ValueType::kBoolean);
		const auto *fallback = std::get_if<bool>(&option.fallback);
		if (fallback == nullptr) {
			std::abort();
		}
		return reader.GetBool(std::string(option.section), std::string(option.key), *fallback);
	}

	inline auto ReadRuntimeInt(const ConfigReader &reader, config_schema::OptionId id) -> int {
		const auto &option   = RuntimeOption(id, config_schema::ValueType::kInteger);
		const auto *fallback = std::get_if<int>(&option.fallback);
		if (fallback == nullptr) {
			std::abort();
		}
		const int value =
		    reader.GetInt(std::string(option.section), std::string(option.key), *fallback);
		if (const auto *range = std::get_if<config_schema::IntegerRange>(&option.range)) {
			if (range->allowed_value.has_value() && value == *range->allowed_value) {
				return value;
			}
			return value >= range->minimum && value <= range->maximum ? value : *fallback;
		}
		return value;
	}

	inline auto ReadRuntimeFloat(const ConfigReader &reader, config_schema::OptionId id) -> float {
		const auto &option   = RuntimeOption(id, config_schema::ValueType::kFloatingPoint);
		const auto *fallback = std::get_if<float>(&option.fallback);
		if (fallback == nullptr) {
			std::abort();
		}
		const float value =
		    reader.GetFloat(std::string(option.section), std::string(option.key), *fallback);
		if (const auto *range = std::get_if<config_schema::FloatRange>(&option.range)) {
			return value >= range->minimum && value <= range->maximum ? value : *fallback;
		}
		return value;
	}

	inline auto ReadRuntimeString(const ConfigReader &reader, config_schema::OptionId id)
	    -> std::string {
		const auto &option   = RuntimeOption(id, config_schema::ValueType::kString);
		const auto *fallback = std::get_if<std::string_view>(&option.fallback);
		if (fallback == nullptr) {
			std::abort();
		}
		auto value = reader.Get(std::string(option.section), std::string(option.key),
		                        std::string(*fallback));
		if (option.choices.empty() ||
		    option.special_rule == config_schema::SpecialRule::kDevicePath) {
			return value;
		}

		std::ranges::transform(value, value.begin(), [](unsigned char ch) -> char {
			return static_cast<char>(std::tolower(ch));
		});
		return std::ranges::find(option.choices, value) != option.choices.end()
		           ? value
		           : std::string(*fallback);
	}

	inline auto ReadSfaceMetric(const ConfigReader &reader) -> std::optional<FaceMetric> {
		const auto &option   = RuntimeOption(config_schema::OptionId::kFaceSfaceMetric,
		                                     config_schema::ValueType::kString);
		const auto *fallback = std::get_if<std::string_view>(&option.fallback);
		if (fallback == nullptr) {
			std::abort();
		}
		const auto value = reader.Get(std::string(option.section), std::string(option.key),
		                              std::string(*fallback));
		return ParseFaceMetric(value.empty() ? *fallback : value);
	}

	inline auto ReadSfaceThreshold(const ConfigReader &reader, FaceMetric metric) -> float {
		const auto &option   = RuntimeOption(config_schema::OptionId::kFaceSfaceThreshold,
		                                     config_schema::ValueType::kFloatingPoint);
		const auto *policy   = GetFaceMetricPolicy(metric);
		const auto *fallback = std::get_if<float>(&option.fallback);
		if (option.special_rule != config_schema::SpecialRule::kSfaceThreshold ||
		    fallback == nullptr || policy == nullptr) {
			std::abort();
		}
		const float value =
		    reader.GetFloat(std::string(option.section), std::string(option.key), *fallback);
		const auto *range   = std::get_if<config_schema::FloatRange>(&option.range);
		const float minimum = range != nullptr ? range->minimum : 0.0F;
		return value >= minimum && value <= policy->threshold_maximum ? value : *fallback;
	}

}  // namespace howdy::native
