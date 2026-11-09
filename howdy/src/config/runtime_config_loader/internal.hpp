#pragma once

#include "../config_document/internal.hpp"
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

	inline auto ReadRuntimeBool(const ConfigDocument &reader, config_schema::OptionId id) -> bool {
		const auto &option   = RuntimeOption(id, config_schema::ValueType::kBoolean);
		const auto *fallback = std::get_if<bool>(&option.fallback);
		if (fallback == nullptr) {
			std::abort();
		}
		return reader.GetBool(option.section, option.key, *fallback);
	}

	inline auto ReadRuntimeInt(const ConfigDocument &reader, config_schema::OptionId id) -> int {
		const auto &option   = RuntimeOption(id, config_schema::ValueType::kInteger);
		const auto *fallback = std::get_if<int>(&option.fallback);
		if (fallback == nullptr) {
			std::abort();
		}
		const int value = reader.GetInt(option.section, option.key, *fallback);
		if (const auto *range = std::get_if<config_schema::IntegerRange>(&option.range)) {
			if (range->allowed_value.has_value() && value == *range->allowed_value) {
				return value;
			}
			return value >= range->minimum && value <= range->maximum ? value : *fallback;
		}
		return value;
	}

	inline auto ReadRuntimeFloat(const ConfigDocument &reader, config_schema::OptionId id)
	    -> float {
		const auto &option   = RuntimeOption(id, config_schema::ValueType::kFloatingPoint);
		const auto *fallback = std::get_if<float>(&option.fallback);
		if (fallback == nullptr) {
			std::abort();
		}
		const float value = reader.GetFloat(option.section, option.key, *fallback);
		if (const auto *range = std::get_if<config_schema::FloatRange>(&option.range)) {
			return value >= range->minimum && value <= range->maximum ? value : *fallback;
		}
		return value;
	}

	inline auto ReadRuntimeString(const ConfigDocument &reader, config_schema::OptionId id)
	    -> std::string {
		const auto &option   = RuntimeOption(id, config_schema::ValueType::kString);
		const auto *fallback = std::get_if<std::string_view>(&option.fallback);
		if (fallback == nullptr) {
			std::abort();
		}
		auto value = reader.GetString(option.section, option.key, std::string(*fallback));
		if (option.choices.empty() ||
		    option.special_rule == config_schema::SpecialRule::kDevicePath) {
			return value;
		}

		return std::ranges::find(option.choices, value) != option.choices.end()
		           ? value
				   : std::string(*fallback);
	}

	inline auto ReadSfaceMetric(const ConfigDocument &reader) -> std::optional<FaceMetric> {
		const auto &option   = RuntimeOption(config_schema::OptionId::kFaceSfaceMetric,
		                                     config_schema::ValueType::kString);
		const auto *fallback = std::get_if<std::string_view>(&option.fallback);
		if (fallback == nullptr) {
			std::abort();
		}
		const auto value = reader.GetString(option.section, option.key, std::string(*fallback));
		return ParseFaceMetric(value);
	}

	inline auto ReadSfaceThreshold(const ConfigDocument &reader, FaceMetric metric) -> float {
		const auto &option   = RuntimeOption(config_schema::OptionId::kFaceSfaceThreshold,
		                                     config_schema::ValueType::kFloatingPoint);
		const auto *policy   = GetFaceMetricPolicy(metric);
		const auto *fallback = std::get_if<float>(&option.fallback);
		if (option.special_rule != config_schema::SpecialRule::kSfaceThreshold ||
		    fallback == nullptr || policy == nullptr) {
			std::abort();
		}
		const float value   = reader.GetFloat(option.section, option.key, *fallback);
		const auto *range   = std::get_if<config_schema::FloatRange>(&option.range);
		const float minimum = range != nullptr ? range->minimum : 0.0F;
		return value >= minimum && value <= policy->threshold_maximum ? value : *fallback;
	}

}  // namespace howdy::native
