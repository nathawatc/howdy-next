#include "app/command_catalog.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

namespace howdy::native {

	namespace {

		constexpr std::array<CommandOptionDescriptor, 1> kTestCommandOptions = {{
		    {
		        .short_name    = "",
		        .long_name     = "--device",
		        .argument_name = "DEVICE",
		        .summary       = "Camera device path",
		    },
		}};

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
		            .min_positionals   = 0,
		            .max_positionals   = 1,
		            .global_options    = GlobalOptionBit(GlobalOptionId::kUser) |
		                                 GlobalOptionBit(GlobalOptionId::kPlain) |
		                                 GlobalOptionBit(GlobalOptionId::kYes),
		        },
		        {
		            .id                = CommandId::kClear,
		            .name              = "clear",
		            .summary           = "Remove all models",
		            .kind              = CommandKind::kEntrypoint,
		            .user_target       = UserTargetMode::kModelUser,
		            .completion        = CommandCompletionKind::kNone,
		            .argument_synopsis = "",
		            .min_positionals   = 0,
		            .max_positionals   = 0,
		            .global_options    = GlobalOptionBit(GlobalOptionId::kUser) |
		                                 GlobalOptionBit(GlobalOptionId::kYes),
		        },
		        {
		            .id                = CommandId::kConfig,
		            .name              = "config",
		            .summary           = "Edit config",
		            .kind              = CommandKind::kEntrypoint,
		            .user_target       = UserTargetMode::kNone,
		            .completion        = CommandCompletionKind::kNone,
		            .argument_synopsis = "",
		            .min_positionals   = 0,
		            .max_positionals   = 0,
		        },
		        {
		            .id                = CommandId::kDisable,
		            .name              = "disable",
		            .summary           = "Enable or disable auth",
		            .kind              = CommandKind::kEntrypoint,
		            .user_target       = UserTargetMode::kNone,
		            .completion        = CommandCompletionKind::kBoolean,
		            .argument_synopsis = "{0|1|true|false}",
		            .min_positionals   = 1,
		            .max_positionals   = 1,
		        },
		        {
		            .id                = CommandId::kDownloadModels,
		            .name              = "download-models",
		            .summary           = "Download ONNX models",
		            .kind              = CommandKind::kEntrypoint,
		            .user_target       = UserTargetMode::kNone,
		            .completion        = CommandCompletionKind::kNone,
		            .argument_synopsis = "",
		            .min_positionals   = 0,
		            .max_positionals   = 0,
		        },
		        {
		            .id                = CommandId::kList,
		            .name              = "list",
		            .summary           = "List models",
		            .kind              = CommandKind::kEntrypoint,
		            .user_target       = UserTargetMode::kModelUser,
		            .completion        = CommandCompletionKind::kNone,
		            .argument_synopsis = "",
		            .min_positionals   = 0,
		            .max_positionals   = 0,
		            .global_options    = GlobalOptionBit(GlobalOptionId::kUser) |
		                                 GlobalOptionBit(GlobalOptionId::kPlain),
		        },
		        {
		            .id                = CommandId::kRemove,
		            .name              = "remove",
		            .summary           = "Remove a specific model",
		            .kind              = CommandKind::kEntrypoint,
		            .user_target       = UserTargetMode::kModelUser,
		            .completion        = CommandCompletionKind::kNone,
		            .argument_synopsis = "ID",
		            .min_positionals   = 1,
		            .max_positionals   = 1,
		            .global_options    = GlobalOptionBit(GlobalOptionId::kUser) |
		                                 GlobalOptionBit(GlobalOptionId::kYes),
		        },
		        {
		            .id                = CommandId::kSet,
		            .name              = "set",
		            .summary           = "Edit config value",
		            .kind              = CommandKind::kEntrypoint,
		            .user_target       = UserTargetMode::kNone,
		            .completion        = CommandCompletionKind::kConfigSet,
		            .argument_synopsis = "KEY VALUE",
		            .min_positionals   = 2,
		            .max_positionals   = 2,
		        },
		        {
		            .id                = CommandId::kSnapshot,
		            .name              = "snapshot",
		            .summary           = "Capture and save a camera snapshot",
		            .kind              = CommandKind::kEntrypoint,
		            .user_target       = UserTargetMode::kNone,
		            .completion        = CommandCompletionKind::kNone,
		            .argument_synopsis = "",
		            .min_positionals   = 0,
		            .max_positionals   = 0,
		        },
		        {
		            .id                = CommandId::kTest,
		            .name              = "test",
		            .summary           = "Test camera",
		            .kind              = CommandKind::kEntrypoint,
		            .user_target       = UserTargetMode::kModelUser,
		            .completion        = CommandCompletionKind::kNone,
		            .argument_synopsis = "[--device DEVICE]",
		            .min_positionals   = 0,
		            .max_positionals   = 0,
		            .global_options    = GlobalOptionBit(GlobalOptionId::kUser),
		            .options           = kTestCommandOptions,
		        },
		        {
		            .id                = CommandId::kVersion,
		            .name              = "version",
		            .summary           = "Print version",
		            .kind              = CommandKind::kVersion,
		            .user_target       = UserTargetMode::kNone,
		            .completion        = CommandCompletionKind::kNone,
		            .argument_synopsis = "",
		            .min_positionals   = 0,
		            .max_positionals   = 0,
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

	auto CommandCatalog() -> std::span<const CommandDescriptor> {
		return kCommandCatalog;
	}

	auto GlobalOptionCatalog() -> std::span<const GlobalOptionDescriptor> {
		return kGlobalOptionCatalog;
	}

	auto FindCommand(std::string_view name) -> const CommandDescriptor * {
		for (const auto &descriptor : kCommandCatalog) {
			if (descriptor.name == name) {
				return &descriptor;
			}
		}
		return nullptr;
	}

	auto CommandAcceptsGlobalOption(const CommandDescriptor &command, GlobalOptionId id) -> bool {
		return (command.global_options & GlobalOptionBit(id)) != 0;
	}

	auto FindCommandOption(const CommandDescriptor &command, std::string_view spelling)
	    -> const CommandOptionDescriptor * {
		for (const auto &option : command.options) {
			if ((!option.short_name.empty() && option.short_name == spelling) ||
			    (!option.long_name.empty() && option.long_name == spelling)) {
				return &option;
			}
		}
		return nullptr;
	}

	auto FindGlobalOption(GlobalOptionId id) -> const GlobalOptionDescriptor * {
		for (const auto &descriptor : kGlobalOptionCatalog) {
			if (descriptor.id == id) {
				return &descriptor;
			}
		}
		return nullptr;
	}

	auto FindGlobalOption(std::string_view spelling) -> const GlobalOptionDescriptor * {
		for (const auto &descriptor : kGlobalOptionCatalog) {
			if ((!descriptor.short_name.empty() && descriptor.short_name == spelling) ||
			    (!descriptor.long_name.empty() && descriptor.long_name == spelling)) {
				return &descriptor;
			}
		}
		return nullptr;
	}

}  // namespace howdy::native
