#include "config/config_schema.hpp"
#include "internal.hpp"
#include "support/face_metric.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace howdy::native::config_schema_internal {

	namespace {

		using config_schema::FloatRange;
		using config_schema::IntegerRange;
		using config_schema::NumericRange;
		using config_schema::Option;
		using config_schema::OptionId;
		using config_schema::OptionName;
		using config_schema::SpecialRule;
		using config_schema::ValueType;

		auto IsKnownValueType(ValueType type) -> bool {
			switch (type) {
				case ValueType::kBoolean:
				case ValueType::kInteger:
				case ValueType::kFloatingPoint:
				case ValueType::kString:
					return true;
			}
			return false;
		}

		auto FallbackMatchesType(const Option &option) -> bool {
			switch (option.type) {
				case ValueType::kBoolean:
					return std::holds_alternative<bool>(option.fallback);
				case ValueType::kInteger:
					return std::holds_alternative<int>(option.fallback);
				case ValueType::kFloatingPoint:
					return std::holds_alternative<float>(option.fallback);
				case ValueType::kString:
					return std::holds_alternative<std::string_view>(option.fallback);
			}
			return false;
		}

		auto ValidateIntegerFallback(const Option &option, const IntegerRange &range)
		    -> std::optional<std::string> {
			const auto *fallback = std::get_if<int>(&option.fallback);
			if (fallback == nullptr) {
				return "fallback type mismatch: " + OptionName(option);
			}
			if (!RangeContains(range, *fallback)) {
				return "integer fallback is outside range: " + OptionName(option);
			}
			return std::nullopt;
		}

		auto ValidateRange(const Option &option) -> std::optional<std::string> {
			const auto name = OptionName(option);
			if (option.type != ValueType::kInteger && option.type != ValueType::kFloatingPoint) {
				if (!std::holds_alternative<std::monostate>(option.range)) {
					return "numeric range is incompatible with option type: " + name;
				}
				return std::nullopt;
			}

			if (option.type == ValueType::kInteger) {
				if (std::holds_alternative<FloatRange>(option.range)) {
					return "numeric range is incompatible with option type: " + name;
				}
				if (const auto *range = std::get_if<IntegerRange>(&option.range)) {
					if (range->minimum > range->maximum) {
						return "invalid numeric range: " + name;
					}
					return ValidateIntegerFallback(option, *range);
				}
				return std::nullopt;
			}

			if (std::holds_alternative<IntegerRange>(option.range)) {
				return "numeric range is incompatible with option type: " + name;
			}
			const auto *fallback = std::get_if<float>(&option.fallback);
			if (fallback == nullptr || !std::isfinite(*fallback)) {
				return "floating-point fallback is not finite: " + name;
			}
			if (const auto *range = std::get_if<FloatRange>(&option.range)) {
				if (!std::isfinite(range->minimum) || !std::isfinite(range->maximum) ||
				    range->minimum > range->maximum) {
					return "invalid numeric range: " + name;
				}
				if (!RangeContains(*range, *fallback)) {
					return "floating-point fallback is outside range: " + name;
				}
			}
			return std::nullopt;
		}

		auto ValidateChoices(const Option &option) -> std::optional<std::string> {
			if (option.choices.empty()) {
				return std::nullopt;
			}
			const auto name = OptionName(option);
			if (option.type != ValueType::kString) {
				return "choices are incompatible with option type: " + name;
			}
			for (const auto [choice_index, choice] : std::views::enumerate(option.choices)) {
				if (choice.empty()) {
					return "choice is empty: " + name;
				}
				const auto previous_choices =
				    option.choices.first(static_cast<std::size_t>(choice_index));
				if (std::ranges::find(previous_choices, choice) != previous_choices.end()) {
					return "duplicate choice: " + name;
				}
			}
			const auto *fallback_str = std::get_if<std::string_view>(&option.fallback);
			if (option.special_rule != SpecialRule::kDevicePath && fallback_str != nullptr &&
			    std::ranges::find(option.choices, *fallback_str) == option.choices.end()) {
				return "fallback is not one of choices: " + name;
			}
			return std::nullopt;
		}

		auto ValidateSpecialRule(const Option &option) -> std::optional<std::string> {
			const auto name = OptionName(option);
			switch (option.special_rule) {
				case SpecialRule::kNone:
					return std::nullopt;
				case SpecialRule::kDevicePath:
					if (option.type == ValueType::kString) {
						return std::nullopt;
					}
					return "special rule/type mismatch: " + name;
				case SpecialRule::kSfaceThreshold:
					if (option.type == ValueType::kFloatingPoint) {
						return std::nullopt;
					}
					return "special rule/type mismatch: " + name;
			}
			return "unknown special rule: " + name;
		}

		auto ValidateSpecialFallbacks(std::span<const Option> options)
		    -> std::optional<std::string> {
			const Option *metric_option = nullptr;
			for (const auto &option : options) {
				if (option.id == OptionId::kFaceSfaceMetric) {
					metric_option = &option;
					break;
				}
			}
			if (metric_option == nullptr) {
				return std::nullopt;
			}
			const auto *metric_str = std::get_if<std::string_view>(&metric_option->fallback);
			if (metric_str == nullptr) {
				return std::nullopt;
			}
			const auto metric = ParseFaceMetric(*metric_str);
			if (!metric.has_value()) {
				return std::nullopt;
			}
			const auto *policy = GetFaceMetricPolicy(*metric);
			if (policy == nullptr) {
				return std::nullopt;
			}

			for (const auto &option : options) {
				if (option.special_rule == SpecialRule::kSfaceThreshold) {
					const auto *threshold = std::get_if<float>(&option.fallback);
					if (threshold != nullptr && !FaceMetricThresholdAllowed(*policy, *threshold)) {
						return "sface threshold fallback outside " + std::string(policy->spelling) +
						       " range: " + OptionName(option);
					}
				}
			}
			return std::nullopt;
		}

		auto ValidateIdentity(const Option &option, std::span<const Option> previous_options)
		    -> std::optional<std::string> {
			const auto name = OptionName(option);
			if (std::to_underlying(option.id) >= std::to_underlying(OptionId::kCount)) {
				return "option has invalid id: " + name;
			}
			if (option.section.empty()) {
				return "invalid or empty section: " + name;
			}
			if (option.key.empty()) {
				return "invalid or empty key: " + name;
			}
			for (const auto &previous : previous_options) {
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

	auto ValidateSchemaOptions(std::span<const Option> options) -> std::optional<std::string> {
		for (const auto [index, option] : std::views::enumerate(options)) {
			const auto name = OptionName(option);
			if (const auto error =
			        ValidateIdentity(option, options.first(static_cast<std::size_t>(index)))) {
				return error;
			}
			if (!IsKnownValueType(option.type)) {
				return "unknown option type: " + name;
			}
			if (!FallbackMatchesType(option)) {
				return "fallback type mismatch: " + name;
			}
			if (const auto error = ValidateRange(option)) {
				return error;
			}
			if (const auto error = ValidateChoices(option)) {
				return error;
			}
			if (const auto error = ValidateSpecialRule(option)) {
				return error;
			}
		}
		if (const auto error = ValidateSpecialFallbacks(options)) {
			return error;
		}
		return std::nullopt;
	}

}  // namespace howdy::native::config_schema_internal
