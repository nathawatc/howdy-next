#include "config/config_schema.hpp"
#include "config_document/internal.hpp"
#include "config_validation/internal.hpp"
#include "support/capture_device_path.hpp"
#include "support/face_metric.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace howdy::native {
	auto InvalidConfigValueMessage(std::string_view key, std::string_view value,
	                               std::string_view rule) -> std::string {
		return "Invalid config value for " + std::string(key) + "=\"" + std::string(value) +
		       "\": " + std::string(rule);
	}

	namespace {

		auto InvalidValue(const config_schema::Option      &option,
		                  const config_schema::ConfigValue &value, std::string_view rule)
		    -> std::string {
			return InvalidConfigValueMessage(option.key, config_schema::FormatConfigValue(value),
			                                 rule);
		}

		auto ValidateInteger(const config_schema::Option &option, int value)
		    -> std::optional<std::string> {
			const auto *range = std::get_if<config_schema::IntegerRange>(&option.range);
			if (range == nullptr ||
			    (range->allowed_value.has_value() && value == *range->allowed_value) ||
			    (value >= range->minimum && value <= range->maximum)) {
				return std::nullopt;
			}
			return InvalidValue(option, config_schema::ConfigValue{value}, option.invalid_rule);
		}

		auto ValidateSfaceThreshold(const ConfigDocument        &config,
		                            const config_schema::Option &option, float value, float minimum)
		    -> std::optional<std::string> {
			const auto &metric_option =
			    config_schema::RuntimeConfigOption(config_schema::OptionId::kFaceSfaceMetric);
			const auto metric_value = config.GetString(
			    metric_option.section, metric_option.key,
			    std::string(config_schema::RuntimeDefaultString(metric_option.id)));
			const auto  metric = ParseFaceMetric(metric_value);
			const auto *policy = metric.has_value() ? GetFaceMetricPolicy(*metric) : nullptr;
			if (policy == nullptr) {
				return InvalidConfigValueMessage(metric_option.key, metric_value,
				                                 metric_option.invalid_rule);
			}
			if (value >= minimum && value <= policy->threshold_maximum) {
				return std::nullopt;
			}
			const auto maximum_text =
			    config_schema::FormatFloatingPointValue(policy->threshold_maximum).value_or("");
			return InvalidValue(option, config_schema::ConfigValue{value},
			                    "expected range 0.." + maximum_text);
		}

		auto ValidateFloat(const ConfigDocument &config, const config_schema::Option &option,
		                   float value) -> std::optional<std::string> {
			const auto *range   = std::get_if<config_schema::FloatRange>(&option.range);
			const float minimum = range != nullptr ? range->minimum : 0.0F;
			if (option.special_rule == config_schema::SpecialRule::kSfaceThreshold) {
				return ValidateSfaceThreshold(config, option, value, minimum);
			}
			if (range != nullptr && (value < range->minimum || value > range->maximum)) {
				return InvalidValue(option, config_schema::ConfigValue{value}, option.invalid_rule);
			}
			return std::nullopt;
		}

		auto ValidateString(const config_schema::Option &option, const std::string &value)
		    -> std::optional<std::string> {
			const bool listed = std::ranges::find(option.choices, value) != option.choices.end();
			if (option.special_rule == config_schema::SpecialRule::kDevicePath) {
				if (listed || IsAllowedCaptureDevicePath(value)) {
					return std::nullopt;
				}
				return InvalidConfigValueMessage(option.key, value, option.invalid_rule);
			}
			if (!option.choices.empty() && !listed) {
				return InvalidConfigValueMessage(option.key, value, option.invalid_rule);
			}
			return std::nullopt;
		}

		// ConfigDocument already guarantees each value has its option's type; only ranges,
		// choices, and cross-option rules remain.
		auto ValidateKnownConfigValue(const ConfigDocument             &config,
		                              const config_schema::Option      &option,
		                              const config_schema::ConfigValue &value)
		    -> std::optional<std::string> {
			if (const auto *number = std::get_if<int>(&value)) {
				return ValidateInteger(option, *number);
			}
			if (const auto *number = std::get_if<float>(&value)) {
				return ValidateFloat(config, option, *number);
			}
			if (const auto *text = std::get_if<std::string>(&value)) {
				return ValidateString(option, *text);
			}
			return std::nullopt;
		}

	}  // namespace

	auto ValidateRuntimeConfig(const ConfigDocument &config) -> std::optional<std::string> {
		for (const auto &option : config_schema::RuntimeConfigOptions()) {
			const auto *value = config.Value(option.section, option.key);
			if (value == nullptr) {
				continue;
			}
			if (const auto validation = ValidateKnownConfigValue(config, option, *value)) {
				return validation;
			}
		}
		return std::nullopt;
	}

}  // namespace howdy::native
