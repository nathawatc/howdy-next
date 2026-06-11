#pragma once

#include <span>
#include <string_view>

namespace howdy::native::config_schema {

	enum class ValueType {
		boolean,
		integer,
		floating_point,
		string,
	};

	enum class SpecialRule {
		none,
		device_path,
		model_path,
		sface_threshold,
	};

	struct NumericRange {
		float minimum;
		float maximum;
		bool  has_allowed_value = false;
		float allowed_value     = 0.0F;
	};

	struct RuntimeDefault {
		bool             has_boolean;
		bool             boolean;
		bool             has_integer;
		int              integer;
		bool             has_floating_point;
		float            floating_point;
		bool             has_string;
		std::string_view string;
	};

	struct Option {
		std::string_view                  section;
		std::string_view                  key;
		ValueType                         type;
		RuntimeDefault                    fallback;
		NumericRange                      range;
		std::span<const std::string_view> choices;
		SpecialRule                       special_rule;
		std::string_view                  invalid_rule;
	};

	constexpr auto bool_default(bool value) -> RuntimeDefault {
		return {.has_boolean = true, .boolean = value};
	}

	constexpr auto int_default(int value) -> RuntimeDefault {
		return {.has_integer = true, .integer = value};
	}

	constexpr auto float_default(float value) -> RuntimeDefault {
		return {.has_floating_point = true, .floating_point = value};
	}

	constexpr auto string_default(std::string_view value) -> RuntimeDefault {
		return {.has_string = true, .string = value};
	}

	inline constexpr float sface_cosine_threshold_default = 0.363F;
	inline constexpr float sface_other_threshold_default  = 1.128F;
	inline constexpr float sface_cosine_threshold_maximum = 1.0F;

	auto runtime_config_options() -> std::span<const Option>;
	auto runtime_config_option(std::string_view section, std::string_view key) -> const Option *;

}  // namespace howdy::native::config_schema
