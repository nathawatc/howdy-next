#include "docs/man_reference.hpp"

#include "vision/capture_device_path.hpp"

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

		auto failure(std::string message) -> RenderResult {
			return {.output = {}, .error = std::move(message)};
		}

		auto has_unsafe_text(std::string_view value) -> bool {
			return std::ranges::any_of(value, [](const char raw_character) -> bool {
				const auto character = static_cast<unsigned char>(raw_character);
				return character < 0x20U || character == 0x7fU;
			});
		}

		auto validate_text(TextField text) -> std::optional<std::string> {
			if (text.value.empty()) {
				return std::string(text.kind) + " is empty";
			}
			if (has_unsafe_text(text.value)) {
				return std::string(text.kind) + " contains a control character";
			}
			return std::nullopt;
		}

		auto validate_roff_text(TextField text) -> std::optional<std::string> {
			if (has_unsafe_text(text.value)) {
				return std::string(text.kind) + " contains a control character";
			}
			return std::nullopt;
		}

		auto escape_roff(std::string_view value) -> std::string {
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

		void append_entry(std::string &output, ReferenceEntry entry) {
			output += ".TP\n\\&\\fB";
			output += escape_roff(entry.label);
			output += "\\fR\n\\&";
			output += escape_roff(entry.summary);
			output += '\n';
		}

		auto finish(std::string output) -> RenderResult {
			while (!output.empty() && output.back() == '\n') {
				output.pop_back();
			}
			output.push_back('\n');
			return {.output = std::move(output), .error = {}};
		}

		auto format_command_label(const native::CommandDescriptor &command) -> std::string {
			std::string label(command.name);
			if (!command.argument_synopsis.empty()) {
				label += ' ';
				label += command.argument_synopsis;
			}
			return label;
		}

		auto format_option_label(const native::GlobalOptionDescriptor &option) -> std::string {
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

		auto validate_command_texts(std::span<const native::CommandDescriptor> commands)
		    -> std::optional<std::string> {
			for (const auto &command : commands) {
				if (const auto error =
				        validate_roff_text({.kind = "command name", .value = command.name})) {
					return error;
				}
				if (const auto error =
				        validate_roff_text({.kind = "command summary", .value = command.summary})) {
					return error;
				}
				if (const auto error = validate_roff_text({.kind  = "command argument synopsis",
				                                           .value = command.argument_synopsis})) {
					return error;
				}
			}
			return std::nullopt;
		}

		auto validate_option_texts(std::span<const native::GlobalOptionDescriptor> options)
		    -> std::optional<std::string> {
			for (const auto &option : options) {
				if (const auto error = validate_roff_text(
				        {.kind = "global option short spelling", .value = option.short_name})) {
					return error;
				}
				if (const auto error = validate_roff_text(
				        {.kind = "global option long spelling", .value = option.long_name})) {
					return error;
				}
				if (const auto error = validate_roff_text(
				        {.kind = "global option argument name", .value = option.argument_name})) {
					return error;
				}
				if (const auto error = validate_roff_text(
				        {.kind = "global option summary", .value = option.summary})) {
					return error;
				}
			}
			return std::nullopt;
		}

		auto default_workaround_name(std::span<const pam::WorkaroundDescriptor> workarounds,
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

		auto validate_workarounds(std::span<const pam::WorkaroundDescriptor> workarounds)
		    -> std::optional<std::string> {
			if (workarounds.empty()) {
				return "workaround catalog is empty";
			}
			for (std::size_t index = 0; index < workarounds.size(); ++index) {
				const auto &workaround = workarounds[index];
				if (const auto error =
				        validate_text({.kind = "workaround value", .value = workaround.value})) {
					return error;
				}
				if (const auto error = validate_text(
				        {.kind = "workaround summary", .value = workaround.summary})) {
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

		auto format_number(native::config_schema::ValueType type, float value) -> std::string {
			if (type == native::config_schema::ValueType::integer) {
				const auto formatted =
				    native::config_schema::format_integer_value(static_cast<int>(value));
				return formatted ? *formatted : "";
			}
			const auto formatted = native::config_schema::format_floating_point_value(value);
			return formatted ? *formatted : "";
		}

		auto format_boolean_values(const native::config_schema::Option &option) -> std::string {
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
					result += escape_roff(spelling);
					result += "\\fR";
				} else {
					result += escape_roff(spelling);
				}
			}
			result += '.';
			return result;
		}

		auto format_choices(std::span<const std::string_view> choices,
		                    std::string_view                  default_choice) -> std::string {
			std::string choices_str = "Choices: ";
			for (std::size_t index = 0; index < choices.size(); ++index) {
				if (index > 0) {
					choices_str += ", ";
				}
				const auto choice = choices[index];
				if (choice == default_choice) {
					choices_str += "\\fB";
					choices_str += escape_roff(choice);
					choices_str += "\\fR";
				} else {
					choices_str += escape_roff(choice);
				}
			}
			choices_str += '.';
			return choices_str;
		}

		auto format_device_paths(std::string_view default_path) -> std::string {
			std::string result = "Accepted: ";
			for (std::size_t index = 0; index < native::kAcceptedCaptureDevicePatterns.size();
			     ++index) {
				if (index > 0) {
					result += ", ";
				}
				const auto pattern = native::kAcceptedCaptureDevicePatterns[index];
				if (pattern == default_path) {
					result += "\\fB";
					result += escape_roff(pattern);
					result += "\\fR";
				} else {
					result += escape_roff(pattern);
				}
			}
			result += '.';
			return result;
		}

		auto format_numeric_range(const native::config_schema::Option &option) -> std::string {
			const auto &range = option.range;
			if (range.minimum == 0.0F && range.maximum == 0.0F && !range.has_allowed_value) {
				return "";
			}
			std::string range_str = "Range: ";
			if (range.has_allowed_value) {
				range_str += escape_roff(format_number(option.type, range.allowed_value));
				range_str += " or ";
			}
			range_str += escape_roff(format_number(option.type, range.minimum));
			range_str += "..";
			range_str += escape_roff(format_number(option.type, range.maximum));
			range_str += '.';
			return range_str;
		}

		auto format_sface_threshold_range(const native::config_schema::Option &option)
		    -> std::string {
			const auto min_str =
			    native::config_schema::format_floating_point_value(option.range.minimum);
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
				    native::config_schema::format_floating_point_value(groups[g_idx].first);
				range_str += escape_roff(min_val);
				range_str += "..";
				range_str += escape_roff(max_str.value_or(""));
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
					range_str += escape_roff(spellings[s_idx]);
				}
			}
			range_str += '.';
			return range_str;
		}

		auto format_accepted_rule(const native::config_schema::Option &option) -> std::string {
			if (option.type == native::config_schema::ValueType::boolean) {
				return format_boolean_values(option);
			}
			if (option.special_rule == native::config_schema::SpecialRule::device_path) {
				return format_device_paths(option.fallback.string);
			}
			if (option.special_rule == native::config_schema::SpecialRule::sface_threshold) {
				return format_sface_threshold_range(option);
			}
			if (!option.choices.empty()) {
				return format_choices(option.choices, option.fallback.string);
			}
			return format_numeric_range(option);
		}

		auto validate_config_option_texts(std::span<const native::config_schema::Option> options)
		    -> std::optional<std::string> {
			for (const auto &option : options) {
				if (const auto error = validate_roff_text(
				        {.kind = "config option section", .value = option.section})) {
					return error;
				}
				if (const auto error =
				        validate_roff_text({.kind = "config option key", .value = option.key})) {
					return error;
				}
				if (const auto error = validate_roff_text(
				        {.kind = "config option description", .value = option.description})) {
					return error;
				}
				if (option.description.empty()) {
					return "config option description is empty";
				}
				for (const auto choice : option.choices) {
					if (const auto error =
					        validate_roff_text({.kind = "config option choice", .value = choice})) {
						return error;
					}
				}
				if (const auto error = validate_roff_text(
				        {.kind = "config option invalid rule", .value = option.invalid_rule})) {
					return error;
				}
			}
			return std::nullopt;
		}

		void append_config_option_entry(std::string                         &output,
		                                const native::config_schema::Option &option) {
			output += ".TP\n\\&\\fB";
			output += escape_roff(option.key);
			output += "\\fR\n\\&";
			output += escape_roff(option.description);
			const auto rule = format_accepted_rule(option);
			if (!rule.empty()) {
				output += "\n.br\n\\&";
				output += rule;
			}
			output += '\n';
		}

	}  // namespace

	auto render_command_reference(std::span<const native::CommandDescriptor> commands)
	    -> RenderResult {
		if (const auto error = native::validate_command_catalog(commands)) {
			return failure(error.value());
		}
		if (const auto error = validate_command_texts(commands)) {
			return failure(error.value());
		}

		std::string output;
		for (const auto &command : commands) {
			const auto label = format_command_label(command);
			append_entry(output, {.label = label, .summary = command.summary});
		}
		return finish(std::move(output));
	}

	auto render_global_option_reference(std::span<const native::GlobalOptionDescriptor> options)
	    -> RenderResult {
		if (const auto error = native::validate_global_option_catalog(options)) {
			return failure(error.value());
		}
		if (const auto error = validate_option_texts(options)) {
			return failure(error.value());
		}

		std::string output;
		for (const auto &option : options) {
			append_entry(output, {.label = format_option_label(option), .summary = option.summary});
		}
		return finish(std::move(output));
	}

	auto render_workaround_reference(std::span<const pam::WorkaroundDescriptor> workarounds,
	                                 pam::Workaround default_mode) -> RenderResult {
		if (const auto error = validate_workarounds(workarounds)) {
			return failure(error.value());
		}
		const auto default_name = default_workaround_name(workarounds, default_mode);
		if (!default_name.has_value()) {
			return failure("default workaround mode is not in the catalog");
		}

		std::string       output;
		const std::string default_summary =
		    "Workaround is " + std::string(default_name.value()) + " when workaround= is omitted.";
		append_entry(output, {.label = "(option omitted)", .summary = default_summary});
		for (const auto &workaround : workarounds) {
			const std::string label =
			    std::string(pam::kWorkaroundOptionPrefix) + std::string(workaround.value);
			append_entry(output, {.label = label, .summary = workaround.summary});
		}
		return finish(std::move(output));
	}

	auto render_config_option_reference(std::span<const native::config_schema::Option> options)
	    -> RenderResult {
		if (options.empty()) {
			return failure("config schema has no options");
		}
		if (const auto error = native::config_schema::validate_options(options)) {
			return failure(*error);
		}
		if (const auto error = validate_config_option_texts(options)) {
			return failure(*error);
		}

		std::vector<std::string_view> seen_sections;
		seen_sections.reserve(options.size());

		std::string      output;
		std::string_view current_section;
		for (const auto &option : options) {
			if (option.section != current_section) {
				if (std::ranges::find(seen_sections, option.section) != seen_sections.end()) {
					return failure("section reused non-contiguously: " +
					               std::string(option.section));
				}
				seen_sections.push_back(option.section);
				current_section = option.section;
				output += ".SS [";
				output += escape_roff(current_section);
				output += "]\n";
			}
			append_config_option_entry(output, option);
		}
		return finish(std::move(output));
	}

}  // namespace howdy::docs
