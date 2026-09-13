#include "config/config_schema.hpp"
#include "config_reader/internal.hpp"
#include "config_validation/internal.hpp"
#include "support/capture_device_path.hpp"
#include "support/face_metric.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <string>

namespace howdy::native {
	namespace {

		auto NormalizedLower(std::string value) -> std::string {
			for (char &ch : value) {
				ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
			}
			return value;
		}

		auto ParseIntStrict(std::string_view value) -> std::optional<int> {
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

		auto IsValidBoolText(std::string_view value) -> bool {
			return config_schema::IsAcceptedBooleanText(value);
		}

		auto InvalidConfigValueMessage(std::string_view key, std::string_view value,
		                               std::string_view rule) -> std::string {
			return "Invalid config value for " + std::string(key) + "=\"" + std::string(value) +
			       "\": " + std::string(rule);
		}

		auto ValidateInteger(const config_schema::Option &option, std::string_view value)
		    -> std::optional<std::string> {
			const auto parsed = ParseIntStrict(value);
			if (!parsed.has_value()) {
				return InvalidConfigValueMessage(option.key, value, option.invalid_rule);
			}
			if (option.range.has_allowed_value &&
			    *parsed == static_cast<int>(option.range.allowed_value)) {
				return std::nullopt;
			}
			if (*parsed < static_cast<int>(option.range.minimum) ||
			    *parsed > static_cast<int>(option.range.maximum)) {
				return InvalidConfigValueMessage(option.key, value, option.invalid_rule);
			}
			return std::nullopt;
		}

		auto ValidateFloat(const ConfigReader &config, const config_schema::Option &option,
		                   std::string_view value) -> std::optional<std::string> {
			const auto parsed = ParseConfigFloatStrict(value);
			if (!parsed.has_value()) {
				return InvalidConfigValueMessage(option.key, value, option.invalid_rule);
			}
			if (option.special_rule == config_schema::SpecialRule::kSfaceThreshold) {
				const auto &metric_option =
				    config_schema::RuntimeConfigOption(config_schema::OptionId::kFaceSfaceMetric);
				const auto metric_value =
				    config.Get(std::string(metric_option.section), std::string(metric_option.key),
				               std::string(config_schema::RuntimeDefaultString(metric_option.id)));
				const auto metric = ParseFaceMetric(
				    metric_value.empty() ? config_schema::RuntimeDefaultString(metric_option.id)
				                         : std::string_view(metric_value));
				if (!metric.has_value()) {
					return InvalidConfigValueMessage(metric_option.key, metric_value,
					                                 metric_option.invalid_rule);
				}
				const auto *policy = GetFaceMetricPolicy(*metric);
				if (policy == nullptr) {
					return InvalidConfigValueMessage(metric_option.key, metric_value,
					                                 metric_option.invalid_rule);
				}
				const float maximum = policy->threshold_maximum;
				if (*parsed < option.range.minimum || *parsed > maximum) {
					const auto *const range_rule = policy->threshold_maximum == option.range.maximum
					                                   ? "expected range 0..4"
					                                   : "expected range 0..1";
					return InvalidConfigValueMessage(option.key, value, range_rule);
				}
				return std::nullopt;
			}
			if (*parsed < option.range.minimum || *parsed > option.range.maximum) {
				return InvalidConfigValueMessage(option.key, value, option.invalid_rule);
			}
			return std::nullopt;
		}

		auto ValidateString(const config_schema::Option &option, std::string_view value)
		    -> std::optional<std::string> {
			if (option.special_rule == config_schema::SpecialRule::kDevicePath) {
				if (std::ranges::find(option.choices, value) != option.choices.end()) {
					return std::nullopt;
				}
				if (value.empty() || !IsAllowedCaptureDevicePath(value)) {
					return InvalidConfigValueMessage(option.key, value, option.invalid_rule);
				}
				return std::nullopt;
			}
			if (!option.choices.empty() &&
			    std::ranges::find(option.choices, NormalizedLower(std::string(value))) ==
			        option.choices.end()) {
				return InvalidConfigValueMessage(option.key, value, option.invalid_rule);
			}
			return std::nullopt;
		}

		auto ValidateKnownConfigValue(const ConfigReader          &config,
		                              const config_schema::Option &option, std::string_view value)
		    -> std::optional<std::string> {
			switch (option.type) {
				case config_schema::ValueType::kBoolean:
					if (!IsValidBoolText(value)) {
						return InvalidConfigValueMessage(option.key, value, option.invalid_rule);
					}
					return std::nullopt;
				case config_schema::ValueType::kInteger: {
					return ValidateInteger(option, value);
				}
				case config_schema::ValueType::kFloatingPoint: {
					return ValidateFloat(config, option, value);
				}
				case config_schema::ValueType::kString:
					return ValidateString(option, value);
			}
			return std::nullopt;
		}

	}  // namespace

	auto ValidateRuntimeConfig(const ConfigReader &config) -> std::optional<std::string> {
		for (const auto &option : config_schema::RuntimeConfigOptions()) {
			const auto value = config.Get(std::string(option.section), std::string(option.key), "");
			if (value.empty()) {
				// Empty values preserve existing behavior: they are treated like unset values
				// and fall back to runtime defaults.
				continue;
			}
			if (const auto validation = ValidateKnownConfigValue(config, option, value)) {
				return validation;
			}
		}

		return std::nullopt;
	}

}  // namespace howdy::native
