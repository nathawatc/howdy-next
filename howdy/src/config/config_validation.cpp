#include "config/config_validation.hpp"

#include "common/capture_device_path.hpp"
#include "config/config_schema.hpp"
#include "config/number_parsing.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <string>

namespace howdy::native {
	namespace {

		auto normalized_lower(std::string value) -> std::string {
			for (char &ch : value) {
				ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
			}
			return value;
		}

		auto parse_int_strict(std::string_view value) -> std::optional<int> {
			if (value.empty()) {
				return std::nullopt;
			}

			errno                 = 0;
			char             *end = nullptr;
			const std::string owned_value(value);
			const auto        parsed = std::strtol(owned_value.c_str(), &end, 10);
			if (errno != 0 || end == nullptr || *end != '\0' ||
			    parsed < std::numeric_limits<int>::min() ||
			    parsed > std::numeric_limits<int>::max()) {
				return std::nullopt;
			}

			return static_cast<int>(parsed);
		}

		auto is_valid_bool_text(std::string_view value) -> bool {
			const auto lowered = normalized_lower(std::string(value));
			return lowered == "true" || lowered == "false" || lowered == "1" || lowered == "0" ||
			       lowered == "yes" || lowered == "no" || lowered == "on" || lowered == "off";
		}

		auto invalid_config_value_message(std::string_view key, std::string_view value,
		                                  std::string_view rule) -> std::string {
			return "Invalid config value for " + std::string(key) + "=\"" + std::string(value) +
			       "\": " + std::string(rule);
		}

		auto validate_integer(const config_schema::Option &option, std::string_view value)
		    -> std::optional<std::string> {
			const auto parsed = parse_int_strict(value);
			if (!parsed.has_value()) {
				return invalid_config_value_message(option.key, value, option.invalid_rule);
			}
			if (option.range.has_allowed_value &&
			    *parsed == static_cast<int>(option.range.allowed_value)) {
				return std::nullopt;
			}
			if (*parsed < static_cast<int>(option.range.minimum) ||
			    *parsed > static_cast<int>(option.range.maximum)) {
				return invalid_config_value_message(option.key, value, option.invalid_rule);
			}
			return std::nullopt;
		}

		auto validate_float(const ConfigReader &config, const config_schema::Option &option,
		                    std::string_view value) -> std::optional<std::string> {
			const auto parsed = parse_config_float_strict(value);
			if (!parsed.has_value()) {
				return invalid_config_value_message(option.key, value, option.invalid_rule);
			}
			if (option.special_rule == config_schema::SpecialRule::sface_threshold) {
				const auto &metric_option = config_schema::runtime_config_option(
				    config_schema::OptionId::face_sface_metric);
				const auto  metric  = normalized_lower(config.get(
				    std::string(metric_option.section), std::string(metric_option.key),
				    std::string(config_schema::runtime_default_string(metric_option.id))));
				const float maximum = metric == "cosine"
				                          ? config_schema::sface_cosine_threshold_maximum
				                          : option.range.maximum;
				if (*parsed < option.range.minimum || *parsed > maximum) {
					return invalid_config_value_message(option.key, value,
					                                    metric == "cosine" ? "expected range 0..1"
					                                                       : "expected range 0..4");
				}
				return std::nullopt;
			}
			if (*parsed < option.range.minimum || *parsed > option.range.maximum) {
				return invalid_config_value_message(option.key, value, option.invalid_rule);
			}
			return std::nullopt;
		}

		auto validate_string(const config_schema::Option &option, std::string_view value)
		    -> std::optional<std::string> {
			if (option.special_rule == config_schema::SpecialRule::device_path) {
				if (std::ranges::find(option.choices, value) != option.choices.end()) {
					return std::nullopt;
				}
				if (value.empty() || !is_allowed_capture_device_path(value)) {
					return invalid_config_value_message(option.key, value, option.invalid_rule);
				}
				return std::nullopt;
			}
			if (!option.choices.empty() &&
			    std::ranges::find(option.choices, normalized_lower(std::string(value))) ==
			        option.choices.end()) {
				return invalid_config_value_message(option.key, value, option.invalid_rule);
			}
			return std::nullopt;
		}

		auto validate_known_config_value(const ConfigReader          &config,
		                                 const config_schema::Option &option,
		                                 std::string_view value) -> std::optional<std::string> {
			switch (option.type) {
				case config_schema::ValueType::boolean:
					if (!is_valid_bool_text(value)) {
						return invalid_config_value_message(option.key, value, option.invalid_rule);
					}
					return std::nullopt;
				case config_schema::ValueType::integer: {
					return validate_integer(option, value);
				}
				case config_schema::ValueType::floating_point: {
					return validate_float(config, option, value);
				}
				case config_schema::ValueType::string:
					return validate_string(option, value);
			}
			return std::nullopt;
		}

	}  // namespace

	auto validate_runtime_config(const ConfigReader &config) -> std::optional<std::string> {
		for (const auto &option : config_schema::runtime_config_options()) {
			const auto value = config.get(std::string(option.section), std::string(option.key), "");
			if (value.empty()) {
				// Empty values preserve existing behavior: they are treated like unset values
				// and fall back to runtime defaults.
				continue;
			}
			if (const auto validation = validate_known_config_value(config, option, value)) {
				return validation;
			}
		}

		return std::nullopt;
	}

}  // namespace howdy::native
