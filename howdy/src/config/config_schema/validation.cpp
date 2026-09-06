#include "config/config_schema.hpp"
#include "internal.hpp"
#include "vision/face_metric.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <string>

namespace howdy::native::config_schema_internal {

	namespace {

		using config_schema::NumericRange;
		using config_schema::Option;
		using config_schema::OptionId;
		using config_schema::SpecialRule;
		using config_schema::ValueType;

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

		auto validate_special_fallbacks(std::span<const Option> options)
		    -> std::optional<std::string> {
			const Option *metric_option = nullptr;
			for (const auto &option : options) {
				if (option.id == OptionId::face_sface_metric) {
					metric_option = &option;
					break;
				}
			}
			if (metric_option == nullptr) {
				return std::nullopt;
			}
			const auto metric = parse_face_metric(metric_option->fallback.string);
			if (!metric.has_value()) {
				return std::nullopt;
			}
			const auto *policy = face_metric_policy(*metric);
			if (policy == nullptr) {
				return std::nullopt;
			}

			for (const auto &option : options) {
				if (option.special_rule == SpecialRule::sface_threshold &&
				    option.fallback.floating_point > policy->threshold_maximum) {
					return "sface threshold fallback exceeds " + std::string(policy->spelling) +
					       " range: " + option_name(option);
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

	auto validate_schema_options(std::span<const Option> options) -> std::optional<std::string> {
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

}  // namespace howdy::native::config_schema_internal
