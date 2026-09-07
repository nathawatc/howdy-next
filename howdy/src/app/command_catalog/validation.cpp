#include "app/command_catalog.hpp"
#include "internal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace howdy::native::command_catalog_internal {

	namespace {

		auto IsKnownCommandKind(CommandKind kind) -> bool {
			switch (kind) {
				case CommandKind::kEntrypoint:
				case CommandKind::kVersion:
					return true;
			}
			return false;
		}

		auto IsKnownUserTargetMode(UserTargetMode mode) -> bool {
			switch (mode) {
				case UserTargetMode::kNone:
				case UserTargetMode::kModelUser:
					return true;
			}
			return false;
		}

		auto IsKnownCommandCompletionKind(CommandCompletionKind kind) -> bool {
			switch (kind) {
				case CommandCompletionKind::kNone:
				case CommandCompletionKind::kBoolean:
				case CommandCompletionKind::kConfigSet:
					return true;
			}
			return false;
		}

		auto IsKnownGlobalOptionCompletionKind(GlobalOptionCompletionKind kind) -> bool {
			switch (kind) {
				case GlobalOptionCompletionKind::kNone:
				case GlobalOptionCompletionKind::kUser:
					return true;
			}
			return false;
		}

		auto CommandName(const CommandDescriptor &command) -> std::string {
			return command.name.empty() ? std::string{"<unnamed>"} : std::string(command.name);
		}

		auto OptionLabel(const GlobalOptionDescriptor &option) -> std::string {
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
			return label.empty() ? std::string{"<unspelled>"} : label;
		}

		auto CommandOptionLabel(const CommandOptionDescriptor &option) -> std::string {
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
			return label.empty() ? std::string{"<unspelled>"} : label;
		}

		template <typename Option>
		auto OptionSpellingsCollide(const Option &left, const Option &right) -> bool {
			return (!left.short_name.empty() &&
			        (left.short_name == right.short_name || left.short_name == right.long_name)) ||
			       (!left.long_name.empty() &&
			        (left.long_name == right.short_name || left.long_name == right.long_name));
		}

		auto MissingIdMessage(std::string_view kind, std::size_t id) -> std::string {
			return std::string{"missing "} + std::string(kind) + " id: " + std::to_string(id);
		}

		auto ValidateCommandDescriptor(
		    std::span<const CommandDescriptor>                             commands,
		    std::array<bool, static_cast<std::size_t>(CommandId::kCount)> &seen_ids,
		    std::size_t index) -> std::optional<std::string> {
			const auto &command = commands[index];
			const auto  id      = static_cast<std::size_t>(command.id);
			const auto  name    = CommandName(command);
			if (id >= seen_ids.size()) {
				return "command has invalid id: " + name;
			}
			if (seen_ids[id]) {
				return "duplicate command id: " + name;
			}
			seen_ids[id] = true;
			if (command.name.empty()) {
				return "command name is empty";
			}
			if (command.summary.empty()) {
				return "command summary is empty: " + name;
			}
			if (!IsKnownCommandKind(command.kind)) {
				return "command has unknown kind: " + name;
			}
			if (!IsKnownUserTargetMode(command.user_target)) {
				return "command has unknown user-target mode: " + name;
			}
			if (!IsKnownCommandCompletionKind(command.completion)) {
				return "command has unknown completion kind: " + name;
			}
			if (command.min_positionals > command.max_positionals) {
				return "command positional bounds are reversed: " + name;
			}
			const auto known_global_options = static_cast<GlobalOptionMask>(
			    (1U << static_cast<unsigned int>(GlobalOptionId::kCount)) - 1U);
			if ((command.global_options & known_global_options) != command.global_options) {
				return "command has unknown global option mask: " + name;
			}
			for (std::size_t option_index = 0; option_index < command.options.size();
			     ++option_index) {
				const auto &option = command.options[option_index];
				if (option.short_name.empty() && option.long_name.empty()) {
					return "command option has no spelling: " + name;
				}
				if (option.summary.empty()) {
					return "command option summary is empty: " + CommandOptionLabel(option);
				}
				if (std::ranges::any_of(command.options.first(option_index),
				                        [&option](const auto &previous) -> bool {
					                        return OptionSpellingsCollide(option, previous);
				                        })) {
					return "duplicate command option spelling: " + CommandOptionLabel(option);
				}
			}

			if (command.kind == CommandKind::kVersion &&
			    command.user_target != UserTargetMode::kNone) {
				return "version command cannot target a user: " + name;
			}
			if (command.kind == CommandKind::kVersion &&
			    command.completion != CommandCompletionKind::kNone) {
				return "version command cannot have completion values: " + name;
			}
			if (command.kind == CommandKind::kVersion &&
			    (command.min_positionals != 0 || command.max_positionals != 0 ||
			     command.global_options != 0 || !command.options.empty())) {
				return "version command cannot accept arguments or options: " + name;
			}
			if (std::ranges::any_of(commands.first(index),
			                        [&command](const auto &previous) -> bool {
				                        return previous.name == command.name;
			                        })) {
				return "duplicate command name: " + name;
			}
			return std::nullopt;
		}

		auto ValidateGlobalOptionDescriptor(
		    std::span<const GlobalOptionDescriptor>                             options,
		    std::array<bool, static_cast<std::size_t>(GlobalOptionId::kCount)> &seen_ids,
		    std::size_t index) -> std::optional<std::string> {
			const auto &option = options[index];
			const auto  id     = static_cast<std::size_t>(option.id);
			const auto  label  = OptionLabel(option);
			if (id >= seen_ids.size()) {
				return "global option has invalid id: " + label;
			}
			if (seen_ids[id]) {
				return "duplicate global option id: " + label;
			}
			seen_ids[id] = true;
			if (option.short_name.empty() && option.long_name.empty()) {
				return "global option has no spelling";
			}
			if (!option.short_name.empty() && option.short_name == option.long_name) {
				return "global option repeats its spelling: " + std::string(option.short_name);
			}
			if (option.summary.empty()) {
				return "global option summary is empty: " + label;
			}
			if (!IsKnownGlobalOptionCompletionKind(option.completion)) {
				return "global option has unknown completion kind: " + label;
			}
			if (option.completion != GlobalOptionCompletionKind::kNone &&
			    option.argument_name.empty()) {
				return "global option completion requires an argument: " + label;
			}
			if (option.completion == GlobalOptionCompletionKind::kUser &&
			    option.id != GlobalOptionId::kUser) {
				return "user completion requires user option: " + label;
			}
			if (option.id == GlobalOptionId::kUser && option.argument_name.empty()) {
				return "user option requires an argument name: " + label;
			}
			if (option.id != GlobalOptionId::kUser && !option.argument_name.empty()) {
				return "global option argument is unsupported: " + label;
			}
			if (std::ranges::any_of(options.first(index), [&option](const auto &previous) -> bool {
				    return OptionSpellingsCollide(option, previous);
			    })) {
				return "duplicate global option spelling: " + label;
			}
			return std::nullopt;
		}

	}  // namespace

	auto ValidateCommands(std::span<const CommandDescriptor> commands, bool require_complete)
	    -> std::optional<std::string> {
		if (commands.empty()) {
			return "command catalog is empty";
		}
		std::array<bool, static_cast<std::size_t>(CommandId::kCount)> seen_ids{};
		for (std::size_t index = 0; index < commands.size(); ++index) {
			if (const auto error = ValidateCommandDescriptor(commands, seen_ids, index)) {
				return error;
			}
		}
		if (require_complete) {
			for (std::size_t id = 0; id < seen_ids.size(); ++id) {
				if (!seen_ids[id]) {
					return MissingIdMessage("command", id);
				}
			}
		}
		return std::nullopt;
	}

	auto ValidateGlobalOptions(std::span<const GlobalOptionDescriptor> options,
	                           bool require_complete) -> std::optional<std::string> {
		if (options.empty()) {
			return "global-option catalog is empty";
		}
		std::array<bool, static_cast<std::size_t>(GlobalOptionId::kCount)> seen_ids{};
		for (std::size_t index = 0; index < options.size(); ++index) {
			if (const auto error = ValidateGlobalOptionDescriptor(options, seen_ids, index)) {
				return error;
			}
		}
		if (require_complete) {
			for (std::size_t id = 0; id < seen_ids.size(); ++id) {
				if (!seen_ids[id]) {
					return MissingIdMessage("global option", id);
				}
			}
		}
		return std::nullopt;
	}

}  // namespace howdy::native::command_catalog_internal

namespace howdy::native {

	auto ValidateCommandCatalog(std::span<const CommandDescriptor> commands, bool require_complete)
	    -> std::optional<std::string> {
		return command_catalog_internal::ValidateCommands(commands, require_complete);
	}

	auto ValidateGlobalOptionCatalog(std::span<const GlobalOptionDescriptor> options,
	                                 bool require_complete) -> std::optional<std::string> {
		return command_catalog_internal::ValidateGlobalOptions(options, require_complete);
	}

}  // namespace howdy::native
