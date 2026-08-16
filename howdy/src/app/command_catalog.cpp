#include "app/command_catalog.hpp"

#include <algorithm>
#include <array>
#include <string>

namespace howdy::native {

	namespace {

		constexpr std::array<CommandDescriptor, static_cast<std::size_t>(CommandId::kCount)>
		    kCommandCatalog = {{
		        {
		            .id                = CommandId::kAdd,
		            .name              = "add",
		            .summary           = "Add face model",
		            .kind              = CommandKind::kEntrypoint,
		            .user_target       = UserTargetMode::kModelUser,
		            .completion        = CommandCompletionKind::kNone,
		            .argument_synopsis = "[LABEL]",
		        },
		        {
		            .id                = CommandId::kClear,
		            .name              = "clear",
		            .summary           = "Remove all models",
		            .kind              = CommandKind::kEntrypoint,
		            .user_target       = UserTargetMode::kModelUser,
		            .completion        = CommandCompletionKind::kNone,
		            .argument_synopsis = "",
		        },
		        {
		            .id                = CommandId::kConfig,
		            .name              = "config",
		            .summary           = "Edit config",
		            .kind              = CommandKind::kEntrypoint,
		            .user_target       = UserTargetMode::kNone,
		            .completion        = CommandCompletionKind::kNone,
		            .argument_synopsis = "",
		        },
		        {
		            .id                = CommandId::kDisable,
		            .name              = "disable",
		            .summary           = "Enable or disable auth",
		            .kind              = CommandKind::kEntrypoint,
		            .user_target       = UserTargetMode::kNone,
		            .completion        = CommandCompletionKind::kBoolean,
		            .argument_synopsis = "{0|1|true|false}",
		        },
		        {
		            .id                = CommandId::kDownloadModels,
		            .name              = "download-models",
		            .summary           = "Download ONNX models",
		            .kind              = CommandKind::kEntrypoint,
		            .user_target       = UserTargetMode::kNone,
		            .completion        = CommandCompletionKind::kNone,
		            .argument_synopsis = "",
		        },
		        {
		            .id                = CommandId::kList,
		            .name              = "list",
		            .summary           = "List models",
		            .kind              = CommandKind::kEntrypoint,
		            .user_target       = UserTargetMode::kModelUser,
		            .completion        = CommandCompletionKind::kNone,
		            .argument_synopsis = "",
		        },
		        {
		            .id                = CommandId::kRemove,
		            .name              = "remove",
		            .summary           = "Remove a specific model",
		            .kind              = CommandKind::kEntrypoint,
		            .user_target       = UserTargetMode::kModelUser,
		            .completion        = CommandCompletionKind::kNone,
		            .argument_synopsis = "ID",
		        },
		        {
		            .id                = CommandId::kSet,
		            .name              = "set",
		            .summary           = "Edit config value",
		            .kind              = CommandKind::kEntrypoint,
		            .user_target       = UserTargetMode::kNone,
		            .completion        = CommandCompletionKind::kConfigSet,
		            .argument_synopsis = "KEY VALUE",
		        },
		        {
		            .id                = CommandId::kSnapshot,
		            .name              = "snapshot",
		            .summary           = "Capture and save a camera snapshot",
		            .kind              = CommandKind::kEntrypoint,
		            .user_target       = UserTargetMode::kNone,
		            .completion        = CommandCompletionKind::kNone,
		            .argument_synopsis = "",
		        },
		        {
		            .id                = CommandId::kTest,
		            .name              = "test",
		            .summary           = "Test camera",
		            .kind              = CommandKind::kEntrypoint,
		            .user_target       = UserTargetMode::kModelUser,
		            .completion        = CommandCompletionKind::kNone,
		            .argument_synopsis = "[--device DEVICE]",
		        },
		        {
		            .id                = CommandId::kVersion,
		            .name              = "version",
		            .summary           = "Print version",
		            .kind              = CommandKind::kVersion,
		            .user_target       = UserTargetMode::kNone,
		            .completion        = CommandCompletionKind::kNone,
		            .argument_synopsis = "",
		        },
		    }};

		constexpr std::array<GlobalOptionDescriptor,
		                     static_cast<std::size_t>(GlobalOptionId::kCount)>
		    kGlobalOptionCatalog = {{
		        {
		            .id                   = GlobalOptionId::kUser,
		            .short_name           = "-U",
		            .long_name            = "--user",
		            .argument_name        = "USER",
		            .summary              = "Target user for model commands",
		            .completion           = GlobalOptionCompletionKind::kUser,
		            .parses_after_command = true,
		        },
		        {
		            .id                   = GlobalOptionId::kPlain,
		            .short_name           = "",
		            .long_name            = "--plain",
		            .argument_name        = "",
		            .summary              = "Disable interactive prompts where supported",
		            .completion           = GlobalOptionCompletionKind::kNone,
		            .parses_after_command = true,
		        },
		        {
		            .id                   = GlobalOptionId::kYes,
		            .short_name           = "-y",
		            .long_name            = "",
		            .argument_name        = "",
		            .summary              = "Assume yes where supported",
		            .completion           = GlobalOptionCompletionKind::kNone,
		            .parses_after_command = true,
		        },
		        {
		            .id                   = GlobalOptionId::kHelp,
		            .short_name           = "-h",
		            .long_name            = "--help",
		            .argument_name        = "",
		            .summary              = "Show this help",
		            .completion           = GlobalOptionCompletionKind::kNone,
		            .parses_after_command = false,
		        },
		    }};

	}  // namespace

	auto command_catalog() -> std::span<const CommandDescriptor> {
		return kCommandCatalog;
	}

	auto global_option_catalog() -> std::span<const GlobalOptionDescriptor> {
		return kGlobalOptionCatalog;
	}

	namespace {

		auto is_known_command_kind(CommandKind kind) -> bool {
			switch (kind) {
				case CommandKind::kEntrypoint:
				case CommandKind::kVersion:
					return true;
			}
			return false;
		}

		auto is_known_user_target_mode(UserTargetMode mode) -> bool {
			switch (mode) {
				case UserTargetMode::kNone:
				case UserTargetMode::kModelUser:
					return true;
			}
			return false;
		}

		auto is_known_command_completion_kind(CommandCompletionKind kind) -> bool {
			switch (kind) {
				case CommandCompletionKind::kNone:
				case CommandCompletionKind::kBoolean:
				case CommandCompletionKind::kConfigSet:
					return true;
			}
			return false;
		}

		auto is_known_global_option_completion_kind(GlobalOptionCompletionKind kind) -> bool {
			switch (kind) {
				case GlobalOptionCompletionKind::kNone:
				case GlobalOptionCompletionKind::kUser:
					return true;
			}
			return false;
		}

		auto command_name(const CommandDescriptor &command) -> std::string {
			return command.name.empty() ? std::string{"<unnamed>"} : std::string(command.name);
		}

		auto option_label(const GlobalOptionDescriptor &option) -> std::string {
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

		auto missing_id_message(std::string_view kind, std::size_t id) -> std::string {
			return std::string{"missing "} + std::string(kind) + " id: " + std::to_string(id);
		}

		auto validate_command_descriptor(
		    std::span<const CommandDescriptor>                             commands,
		    std::array<bool, static_cast<std::size_t>(CommandId::kCount)> &seen_ids,
		    std::size_t index) -> std::optional<std::string> {
			const auto &command = commands[index];
			const auto  id      = static_cast<std::size_t>(command.id);
			const auto  name    = command_name(command);
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
			if (!is_known_command_kind(command.kind)) {
				return "command has unknown kind: " + name;
			}
			if (!is_known_user_target_mode(command.user_target)) {
				return "command has unknown user-target mode: " + name;
			}
			if (!is_known_command_completion_kind(command.completion)) {
				return "command has unknown completion kind: " + name;
			}

			if (command.kind == CommandKind::kVersion &&
			    command.user_target != UserTargetMode::kNone) {
				return "version command cannot target a user: " + name;
			}
			if (command.kind == CommandKind::kVersion &&
			    command.completion != CommandCompletionKind::kNone) {
				return "version command cannot have completion values: " + name;
			}
			if (std::ranges::any_of(commands.first(index),
			                        [&command](const auto &previous) -> bool {
				                        return previous.name == command.name;
			                        })) {
				return "duplicate command name: " + name;
			}
			return std::nullopt;
		}

		auto option_spellings_collide(const GlobalOptionDescriptor &left,
		                              const GlobalOptionDescriptor &right) -> bool {
			return (!left.short_name.empty() &&
			        (left.short_name == right.short_name || left.short_name == right.long_name)) ||
			       (!left.long_name.empty() &&
			        (left.long_name == right.short_name || left.long_name == right.long_name));
		}

		auto validate_global_option_descriptor(
		    std::span<const GlobalOptionDescriptor>                             options,
		    std::array<bool, static_cast<std::size_t>(GlobalOptionId::kCount)> &seen_ids,
		    std::size_t index) -> std::optional<std::string> {
			const auto &option = options[index];
			const auto  id     = static_cast<std::size_t>(option.id);
			const auto  label  = option_label(option);
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
			if (!is_known_global_option_completion_kind(option.completion)) {
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
				    return option_spellings_collide(option, previous);
			    })) {
				return "duplicate global option spelling: " + label;
			}
			return std::nullopt;
		}

	}  // namespace

	auto validate_command_catalog(std::span<const CommandDescriptor> commands,
	                              bool require_complete) -> std::optional<std::string> {
		if (commands.empty()) {
			return "command catalog is empty";
		}
		std::array<bool, static_cast<std::size_t>(CommandId::kCount)> seen_ids{};
		for (std::size_t index = 0; index < commands.size(); ++index) {
			if (const auto error = validate_command_descriptor(commands, seen_ids, index)) {
				return error;
			}
		}
		if (require_complete) {
			for (std::size_t id = 0; id < seen_ids.size(); ++id) {
				if (!seen_ids[id]) {
					return missing_id_message("command", id);
				}
			}
		}
		return std::nullopt;
	}

	auto validate_global_option_catalog(std::span<const GlobalOptionDescriptor> options,
	                                    bool require_complete) -> std::optional<std::string> {
		if (options.empty()) {
			return "global-option catalog is empty";
		}
		std::array<bool, static_cast<std::size_t>(GlobalOptionId::kCount)> seen_ids{};
		for (std::size_t index = 0; index < options.size(); ++index) {
			if (const auto error = validate_global_option_descriptor(options, seen_ids, index)) {
				return error;
			}
		}
		if (require_complete) {
			for (std::size_t id = 0; id < seen_ids.size(); ++id) {
				if (!seen_ids[id]) {
					return missing_id_message("global option", id);
				}
			}
		}
		return std::nullopt;
	}

	auto find_command(std::string_view name) -> const CommandDescriptor * {
		for (const auto &descriptor : kCommandCatalog) {
			if (descriptor.name == name) {
				return &descriptor;
			}
		}
		return nullptr;
	}

	auto find_global_option(GlobalOptionId id) -> const GlobalOptionDescriptor * {
		for (const auto &descriptor : kGlobalOptionCatalog) {
			if (descriptor.id == id) {
				return &descriptor;
			}
		}
		return nullptr;
	}

	auto find_global_option(std::string_view spelling) -> const GlobalOptionDescriptor * {
		for (const auto &descriptor : kGlobalOptionCatalog) {
			if ((!descriptor.short_name.empty() && descriptor.short_name == spelling) ||
			    (!descriptor.long_name.empty() && descriptor.long_name == spelling)) {
				return &descriptor;
			}
		}
		return nullptr;
	}

}  // namespace howdy::native
