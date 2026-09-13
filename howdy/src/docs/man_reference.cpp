#include "docs/man_reference.hpp"

#include "support/capture_device_path.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace howdy::docs {

	namespace {

		struct TextField {
			std::string_view kind;
			std::string_view value;
		};

		struct ReferenceEntry {
			std::string_view label;
			std::string_view summary;
		};

		auto Failure(std::string message) -> RenderResult {
			return {.output = {}, .error = std::move(message)};
		}

		auto HasUnsafeText(std::string_view value) -> bool {
			return std::ranges::any_of(value, [](const char raw_character) -> bool {
				const auto character = static_cast<unsigned char>(raw_character);
				return character < 0x20U || character == 0x7fU;
			});
		}

		auto ValidateText(TextField text) -> std::optional<std::string> {
			if (text.value.empty()) {
				return std::string(text.kind) + " is empty";
			}
			if (HasUnsafeText(text.value)) {
				return std::string(text.kind) + " contains a control character";
			}
			return std::nullopt;
		}

		auto ValidateRoffText(TextField text) -> std::optional<std::string> {
			if (HasUnsafeText(text.value)) {
				return std::string(text.kind) + " contains a control character";
			}
			return std::nullopt;
		}

		auto EscapeRoff(std::string_view value) -> std::string {
			std::string escaped;
			escaped.reserve(value.size());
			for (const char character : value) {
				switch (character) {
					case '\\':
						escaped += "\\\\";
						break;
					case '-':
						escaped += "\\-";
						break;
					default:
						escaped += character;
						break;
				}
			}
			return escaped;
		}

		void AppendEntry(std::string &output, ReferenceEntry entry) {
			output += ".TP\n\\&\\fB";
			output += EscapeRoff(entry.label);
			output += "\\fR\n\\&";
			output += EscapeRoff(entry.summary);
			output += '\n';
		}

		auto Finish(std::string output) -> RenderResult {
			while (!output.empty() && output.back() == '\n') {
				output.pop_back();
			}
			output.push_back('\n');
			return {.output = std::move(output), .error = {}};
		}

		auto FormatCommandLabel(const native::CommandDescriptor &command) -> std::string {
			std::string label(command.name);
			if (!command.argument_synopsis.empty()) {
				label += ' ';
				label += command.argument_synopsis;
			}
			return label;
		}

		auto FormatOptionLabel(const native::GlobalOptionDescriptor &option) -> std::string {
			std::string label;
			if (!option.short_name.empty()) {
				label += option.short_name;
			}
			if (!option.long_name.empty()) {
				if (!label.empty()) {
					label += ", ";
				}
				label += option.long_name;
			}
			if (!option.argument_name.empty()) {
				label += ' ';
				label += option.argument_name;
			}
			return label;
		}

		auto ValidateCommandTexts(std::span<const native::CommandDescriptor> commands)
		    -> std::optional<std::string> {
			for (const auto &command : commands) {
				if (const auto error =
				        ValidateRoffText({.kind = "command name", .value = command.name})) {
					return error;
				}
				if (const auto error =
				        ValidateRoffText({.kind = "command summary", .value = command.summary})) {
					return error;
				}
				if (const auto error = ValidateRoffText({.kind  = "command argument synopsis",
				                                         .value = command.argument_synopsis})) {
					return error;
				}
			}
			return std::nullopt;
		}

		auto ValidateOptionTexts(std::span<const native::GlobalOptionDescriptor> options)
		    -> std::optional<std::string> {
			for (const auto &option : options) {
				if (const auto error = ValidateRoffText(
				        {.kind = "global option short spelling", .value = option.short_name})) {
					return error;
				}
				if (const auto error = ValidateRoffText(
				        {.kind = "global option long spelling", .value = option.long_name})) {
					return error;
				}
				if (const auto error = ValidateRoffText(
				        {.kind = "global option argument name", .value = option.argument_name})) {
					return error;
				}
				if (const auto error = ValidateRoffText(
				        {.kind = "global option summary", .value = option.summary})) {
					return error;
				}
			}
			return std::nullopt;
		}

		auto DefaultWorkaroundName(std::span<const pam::WorkaroundDescriptor> workarounds,
		                           pam::Workaround                            default_mode)
		    -> std::optional<std::string_view> {
			if (default_mode == pam::Workaround::kOff) {
				return "off";
			}
			for (const auto &workaround : workarounds) {
				if (workaround.workaround == default_mode) {
					return workaround.value;
				}
			}
			return std::nullopt;
		}

		auto ValidateWorkarounds(std::span<const pam::WorkaroundDescriptor> workarounds)
		    -> std::optional<std::string> {
			if (workarounds.empty()) {
				return "workaround catalog is empty";
			}
			for (std::size_t index = 0; index < workarounds.size(); ++index) {
				const auto &workaround = workarounds[index];
				if (const auto error =
				        ValidateText({.kind = "workaround value", .value = workaround.value})) {
					return error;
				}
				if (const auto error =
				        ValidateText({.kind = "workaround summary", .value = workaround.summary})) {
					return error;
				}
				if (workaround.workaround == pam::Workaround::kOff) {
					return "off must remain the default, not a mapped workaround value";
				}
				for (std::size_t previous = 0; previous < index; ++previous) {
					if (workarounds[previous].value == workaround.value) {
						return "duplicate workaround value: " + std::string(workaround.value);
					}
					if (workarounds[previous].workaround == workaround.workaround) {
						return "duplicate workaround mode: " + std::string(workaround.value);
					}
				}
			}
			return std::nullopt;
		}

		auto FormatNumber(native::config_schema::ValueType type, float value) -> std::string {
			if (type == native::config_schema::ValueType::kInteger) {
				const auto formatted =
				    native::config_schema::FormatIntegerValue(static_cast<int>(value));
				return formatted ? *formatted : "";
			}
			const auto formatted = native::config_schema::FormatFloatingPointValue(value);
			return formatted ? *formatted : "";
		}

		auto FormatBooleanValues(const native::config_schema::Option &option) -> std::string {
			std::string       result           = "Values: ";
			const auto *const default_spelling = option.fallback.boolean ? "true" : "false";
			for (std::size_t index = 0;
			     index < native::config_schema::kAcceptedBooleanSpellings.size(); ++index) {
				if (index > 0) {
					result += ", ";
				}
				const auto spelling = native::config_schema::kAcceptedBooleanSpellings[index];
				if (spelling == default_spelling) {
					result += "\\fB";
					result += EscapeRoff(spelling);
					result += "\\fR";
				} else {
					result += EscapeRoff(spelling);
				}
			}
			result += '.';
			return result;
		}

		auto FormatChoices(std::span<const std::string_view> choices,
		                   std::string_view                  default_choice) -> std::string {
			std::string choices_str = "Choices: ";
			for (std::size_t index = 0; index < choices.size(); ++index) {
				if (index > 0) {
					choices_str += ", ";
				}
				const auto choice = choices[index];
				if (choice == default_choice) {
					choices_str += "\\fB";
					choices_str += EscapeRoff(choice);
					choices_str += "\\fR";
				} else {
					choices_str += EscapeRoff(choice);
				}
			}
			choices_str += '.';
			return choices_str;
		}

		auto FormatDevicePaths(std::string_view default_path) -> std::string {
			std::string result = "Accepted: ";
			for (std::size_t index = 0; index < native::kAcceptedCaptureDevicePatterns.size();
			     ++index) {
				if (index > 0) {
					result += ", ";
				}
				const auto pattern = native::kAcceptedCaptureDevicePatterns[index];
				if (pattern == default_path) {
					result += "\\fB";
					result += EscapeRoff(pattern);
					result += "\\fR";
				} else {
					result += EscapeRoff(pattern);
				}
			}
			result += '.';
			return result;
		}

		auto FormatNumericRange(const native::config_schema::Option &option) -> std::string {
			const auto &range = option.range;
			if (range.minimum == 0.0F && range.maximum == 0.0F && !range.has_allowed_value) {
				return "";
			}
			std::string range_str = "Range: ";
			if (range.has_allowed_value) {
				range_str += EscapeRoff(FormatNumber(option.type, range.allowed_value));
				range_str += " or ";
			}
			range_str += EscapeRoff(FormatNumber(option.type, range.minimum));
			range_str += "..";
			range_str += EscapeRoff(FormatNumber(option.type, range.maximum));
			range_str += '.';
			return range_str;
		}

		auto FormatSfaceThresholdRange(const native::config_schema::Option &option) -> std::string {
			const auto min_str =
			    native::config_schema::FormatFloatingPointValue(option.range.minimum);
			const auto min_val = min_str ? *min_str : "0";

			std::vector<std::pair<float, std::vector<std::string_view>>> groups;
			for (const auto &policy : native::kFaceMetricPolicies) {
				auto iter = std::ranges::find_if(groups, [&](const auto &group) -> bool {
					return group.first == policy.threshold_maximum;
				});
				if (iter != groups.end()) {
					iter->second.push_back(policy.spelling);
				} else {
					groups.push_back({policy.threshold_maximum, {policy.spelling}});
				}
			}

			std::string range_str = "Range: ";
			for (std::size_t g_idx = 0; g_idx < groups.size(); ++g_idx) {
				if (g_idx > 0) {
					range_str += ", ";
				}
				const auto max_str =
				    native::config_schema::FormatFloatingPointValue(groups[g_idx].first);
				range_str += EscapeRoff(min_val);
				range_str += "..";
				range_str += EscapeRoff(max_str.value_or(""));
				range_str += " for ";
				const auto &spellings = groups[g_idx].second;
				for (std::size_t s_idx = 0; s_idx < spellings.size(); ++s_idx) {
					if (s_idx > 0) {
						if (s_idx + 1 == spellings.size()) {
							range_str += " and ";
						} else {
							range_str += ", ";
						}
					}
					range_str += EscapeRoff(spellings[s_idx]);
				}
			}
			range_str += '.';
			return range_str;
		}

		auto FormatAcceptedRule(const native::config_schema::Option &option) -> std::string {
			if (option.type == native::config_schema::ValueType::kBoolean) {
				return FormatBooleanValues(option);
			}
			if (option.special_rule == native::config_schema::SpecialRule::kDevicePath) {
				return FormatDevicePaths(option.fallback.string);
			}
			if (option.special_rule == native::config_schema::SpecialRule::kSfaceThreshold) {
				return FormatSfaceThresholdRange(option);
			}
			if (!option.choices.empty()) {
				return FormatChoices(option.choices, option.fallback.string);
			}
			return FormatNumericRange(option);
		}

		auto ValidateConfigOptionTexts(std::span<const native::config_schema::Option> options)
		    -> std::optional<std::string> {
			for (const auto &option : options) {
				if (const auto error = ValidateRoffText(
				        {.kind = "config option section", .value = option.section})) {
					return error;
				}
				if (const auto error =
				        ValidateRoffText({.kind = "config option key", .value = option.key})) {
					return error;
				}
				if (const auto error = ValidateRoffText(
				        {.kind = "config option description", .value = option.description})) {
					return error;
				}
				if (option.description.empty()) {
					return "config option description is empty";
				}
				for (const auto choice : option.choices) {
					if (const auto error =
					        ValidateRoffText({.kind = "config option choice", .value = choice})) {
						return error;
					}
				}
				if (const auto error = ValidateRoffText(
				        {.kind = "config option invalid rule", .value = option.invalid_rule})) {
					return error;
				}
			}
			return std::nullopt;
		}

		void AppendConfigOptionEntry(std::string                         &output,
		                             const native::config_schema::Option &option) {
			output += ".TP\n\\&\\fB";
			output += EscapeRoff(option.key);
			output += "\\fR\n\\&";
			output += EscapeRoff(option.description);
			const auto rule = FormatAcceptedRule(option);
			if (!rule.empty()) {
				output += "\n.br\n\\&";
				output += rule;
			}
			output += '\n';
		}

	}  // namespace

	auto RenderCommandReference(std::span<const native::CommandDescriptor> commands)
	    -> RenderResult {
		if (const auto error = native::ValidateCommandCatalog(commands)) {
			return Failure(error.value());
		}
		if (const auto error = ValidateCommandTexts(commands)) {
			return Failure(error.value());
		}

		std::string output;
		for (const auto &command : commands) {
			const auto label = FormatCommandLabel(command);
			AppendEntry(output, {.label = label, .summary = command.summary});
		}
		return Finish(std::move(output));
	}

	auto RenderGlobalOptionReference(std::span<const native::GlobalOptionDescriptor> options)
	    -> RenderResult {
		if (const auto error = native::ValidateGlobalOptionCatalog(options)) {
			return Failure(error.value());
		}
		if (const auto error = ValidateOptionTexts(options)) {
			return Failure(error.value());
		}

		std::string output;
		for (const auto &option : options) {
			AppendEntry(output, {.label = FormatOptionLabel(option), .summary = option.summary});
		}
		return Finish(std::move(output));
	}

	auto RenderWorkaroundReference(std::span<const pam::WorkaroundDescriptor> workarounds,
	                               pam::Workaround default_mode) -> RenderResult {
		if (const auto error = ValidateWorkarounds(workarounds)) {
			return Failure(error.value());
		}
		const auto default_name = DefaultWorkaroundName(workarounds, default_mode);
		if (!default_name.has_value()) {
			return Failure("default workaround mode is not in the catalog");
		}

		std::string       output;
		const std::string default_summary =
		    "Workaround is " + std::string(default_name.value()) + " when workaround= is omitted.";
		AppendEntry(output, {.label = "(option omitted)", .summary = default_summary});
		for (const auto &workaround : workarounds) {
			const std::string label =
			    std::string(pam::kWorkaroundOptionPrefix) + std::string(workaround.value);
			AppendEntry(output, {.label = label, .summary = workaround.summary});
		}
		return Finish(std::move(output));
	}

	auto RenderConfigOptionReference(std::span<const native::config_schema::Option> options)
	    -> RenderResult {
		if (options.empty()) {
			return Failure("config schema has no options");
		}
		if (const auto error = native::config_schema::ValidateOptions(options)) {
			return Failure(*error);
		}
		if (const auto error = ValidateConfigOptionTexts(options)) {
			return Failure(*error);
		}

		std::vector<std::string_view> seen_sections;
		seen_sections.reserve(options.size());

		std::string      output;
		std::string_view current_section;
		for (const auto &option : options) {
			if (option.section != current_section) {
				if (std::ranges::find(seen_sections, option.section) != seen_sections.end()) {
					return Failure("section reused non-contiguously: " +
					               std::string(option.section));
				}
				seen_sections.push_back(option.section);
				current_section = option.section;
				output += ".SS [";
				output += EscapeRoff(current_section);
				output += "]\n";
			}
			AppendConfigOptionEntry(output, option);
		}
		return Finish(std::move(output));
	}

}  // namespace howdy::docs
