#pragma once

#include "config/config_reader.hpp"
#include "config/config_schema.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>

namespace howdy::native {

	inline auto runtime_option(config_schema::OptionId id, config_schema::ValueType type)
	    -> const config_schema::Option & {
		const auto &option = config_schema::runtime_config_option(id);
		if (option.type != type) {
			std::abort();
		}
		return option;
	}

	inline auto read_runtime_bool(const ConfigReader &reader, config_schema::OptionId id) -> bool {
		const auto &option = runtime_option(id, config_schema::ValueType::boolean);
		if (!option.fallback.has_boolean) {
			std::abort();
		}
		return reader.get_bool(std::string(option.section), std::string(option.key),
		                       option.fallback.boolean);
	}

	inline auto read_runtime_int(const ConfigReader &reader, config_schema::OptionId id) -> int {
		const auto &option = runtime_option(id, config_schema::ValueType::integer);
		if (!option.fallback.has_integer) {
			std::abort();
		}
		const int value = reader.get_int(std::string(option.section), std::string(option.key),
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

	inline auto read_runtime_float(const ConfigReader &reader, config_schema::OptionId id)
	    -> float {
		const auto &option = runtime_option(id, config_schema::ValueType::floating_point);
		if (!option.fallback.has_floating_point) {
			std::abort();
		}
		const float value = reader.get_float(std::string(option.section), std::string(option.key),
		                                     option.fallback.floating_point);
		return value >= option.range.minimum && value <= option.range.maximum
		           ? value
		           : option.fallback.floating_point;
	}

	inline auto read_runtime_string(const ConfigReader &reader, config_schema::OptionId id)
	    -> std::string {
		const auto &option = runtime_option(id, config_schema::ValueType::string);
		if (!option.fallback.has_string) {
			std::abort();
		}
		auto value = reader.get(std::string(option.section), std::string(option.key),
		                        std::string(option.fallback.string));
		if (option.choices.empty() ||
		    option.special_rule == config_schema::SpecialRule::device_path) {
			return value;
		}

		std::ranges::transform(value, value.begin(), [](unsigned char ch) -> char {
			return static_cast<char>(std::tolower(ch));
		});
		return std::ranges::find(option.choices, value) != option.choices.end()
		           ? value
		           : std::string(option.fallback.string);
	}

	inline auto read_sface_threshold(const ConfigReader &reader, std::string_view metric) -> float {
		const auto &option = runtime_option(config_schema::OptionId::face_sface_threshold,
		                                    config_schema::ValueType::floating_point);
		if (option.special_rule != config_schema::SpecialRule::sface_threshold ||
		    !option.fallback.has_floating_point) {
			std::abort();
		}
		const float maximum = metric == config_schema::sface_cosine_metric
		                          ? config_schema::sface_cosine_threshold_maximum
		                          : option.range.maximum;
		const float value   = reader.get_float(std::string(option.section), std::string(option.key),
		                                       option.fallback.floating_point);
		return value >= option.range.minimum && value <= maximum ? value
		                                                         : option.fallback.floating_point;
	}

}  // namespace howdy::native
