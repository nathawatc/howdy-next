#include "docs/man_reference.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

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

		auto validate_commands(std::span<const native::CommandDescriptor> commands)
		    -> std::optional<std::string> {
			if (commands.empty()) {
				return "command catalog is empty";
			}
			for (std::size_t index = 0; index < commands.size(); ++index) {
				const auto &command = commands[index];
				if (const auto error =
				        validate_text({.kind = "command name", .value = command.name})) {
					return error;
				}
				if (const auto error =
				        validate_text({.kind = "command summary", .value = command.summary})) {
					return error;
				}
				for (std::size_t previous = 0; previous < index; ++previous) {
					if (commands[previous].id == command.id) {
						return "duplicate command id: " + std::string(command.name);
					}
					if (commands[previous].name == command.name) {
						return "duplicate command name: " + std::string(command.name);
					}
				}
			}
			return std::nullopt;
		}

		auto validate_option_texts(const native::GlobalOptionDescriptor &option)
		    -> std::optional<std::string> {
			if (option.short_name.empty() && option.long_name.empty()) {
				return "global option has no spelling";
			}
			if (option.short_name == option.long_name && !option.short_name.empty()) {
				return "global option repeats its spelling: " + std::string(option.short_name);
			}
			if (!option.short_name.empty()) {
				if (const auto error = validate_text(
				        {.kind = "global option short spelling", .value = option.short_name})) {
					return error;
				}
			}
			if (!option.long_name.empty()) {
				if (const auto error = validate_text(
				        {.kind = "global option long spelling", .value = option.long_name})) {
					return error;
				}
			}
			if (!option.argument_name.empty()) {
				if (const auto error = validate_text(
				        {.kind = "global option argument name", .value = option.argument_name})) {
					return error;
				}
			}
			return validate_text({.kind = "global option summary", .value = option.summary});
		}

		auto has_duplicate_option_spelling(std::span<const native::GlobalOptionDescriptor> options,
		                                   std::size_t                                     index,
		                                   const native::GlobalOptionDescriptor &option) -> bool {
			return std::ranges::any_of(
			    options.first(index), [&option](const auto &previous) -> bool {
				    return (!option.short_name.empty() &&
				            (option.short_name == previous.short_name ||
				             option.short_name == previous.long_name)) ||
				           (!option.long_name.empty() && (option.long_name == previous.short_name ||
				                                          option.long_name == previous.long_name));
			    });
		}

		auto validate_options(std::span<const native::GlobalOptionDescriptor> options)
		    -> std::optional<std::string> {
			if (options.empty()) {
				return "global-option catalog is empty";
			}
			for (std::size_t index = 0; index < options.size(); ++index) {
				const auto &option = options[index];
				if (const auto error = validate_option_texts(option)) {
					return error;
				}
				for (std::size_t previous = 0; previous < index; ++previous) {
					if (options[previous].id == option.id) {
						return "duplicate global option id: " + format_option_label(option);
					}
				}
				if (has_duplicate_option_spelling(options, index, option)) {
					return "duplicate global option spelling: " + format_option_label(option);
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

	}  // namespace

	auto render_command_reference(std::span<const native::CommandDescriptor> commands)
	    -> RenderResult {
		if (const auto error = validate_commands(commands)) {
			return failure(error.value());
		}

		std::string output;
		for (const auto &command : commands) {
			append_entry(output, {.label = command.name, .summary = command.summary});
		}
		return finish(std::move(output));
	}

	auto render_global_option_reference(std::span<const native::GlobalOptionDescriptor> options)
	    -> RenderResult {
		if (const auto error = validate_options(options)) {
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

}  // namespace howdy::docs
