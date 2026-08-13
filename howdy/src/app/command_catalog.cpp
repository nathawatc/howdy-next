#include "app/command_catalog.hpp"

#include <algorithm>
#include <array>

namespace howdy::native {

	namespace {

		constexpr std::array<CommandDescriptor, static_cast<std::size_t>(CommandId::kCount)>
		    kCommandCatalog = {{
		        {
		            .id          = CommandId::kAdd,
		            .name        = "add",
		            .summary     = "Add face model",
		            .kind        = CommandKind::kEntrypoint,
		            .user_target = UserTargetMode::kModelUser,
		            .completion  = CommandCompletionKind::kNone,
		        },
		        {
		            .id          = CommandId::kClear,
		            .name        = "clear",
		            .summary     = "Remove all models",
		            .kind        = CommandKind::kEntrypoint,
		            .user_target = UserTargetMode::kModelUser,
		            .completion  = CommandCompletionKind::kNone,
		        },
		        {
		            .id          = CommandId::kConfig,
		            .name        = "config",
		            .summary     = "Edit config",
		            .kind        = CommandKind::kEntrypoint,
		            .user_target = UserTargetMode::kNone,
		            .completion  = CommandCompletionKind::kNone,
		        },
		        {
		            .id          = CommandId::kDisable,
		            .name        = "disable",
		            .summary     = "Enable or disable auth",
		            .kind        = CommandKind::kEntrypoint,
		            .user_target = UserTargetMode::kNone,
		            .completion  = CommandCompletionKind::kBoolean,
		        },
		        {
		            .id          = CommandId::kDownloadModels,
		            .name        = "download-models",
		            .summary     = "Download ONNX models",
		            .kind        = CommandKind::kEntrypoint,
		            .user_target = UserTargetMode::kNone,
		            .completion  = CommandCompletionKind::kNone,
		        },
		        {
		            .id          = CommandId::kList,
		            .name        = "list",
		            .summary     = "List models",
		            .kind        = CommandKind::kEntrypoint,
		            .user_target = UserTargetMode::kModelUser,
		            .completion  = CommandCompletionKind::kNone,
		        },
		        {
		            .id          = CommandId::kRemove,
		            .name        = "remove",
		            .summary     = "Remove a specific model",
		            .kind        = CommandKind::kEntrypoint,
		            .user_target = UserTargetMode::kModelUser,
		            .completion  = CommandCompletionKind::kNone,
		        },
		        {
		            .id          = CommandId::kSet,
		            .name        = "set",
		            .summary     = "Edit config value",
		            .kind        = CommandKind::kEntrypoint,
		            .user_target = UserTargetMode::kNone,
		            .completion  = CommandCompletionKind::kConfigSet,
		        },
		        {
		            .id          = CommandId::kSnapshot,
		            .name        = "snapshot",
		            .summary     = "Camera preview",
		            .kind        = CommandKind::kEntrypoint,
		            .user_target = UserTargetMode::kNone,
		            .completion  = CommandCompletionKind::kNone,
		        },
		        {
		            .id          = CommandId::kTest,
		            .name        = "test",
		            .summary     = "Test camera",
		            .kind        = CommandKind::kEntrypoint,
		            .user_target = UserTargetMode::kModelUser,
		            .completion  = CommandCompletionKind::kNone,
		        },
		        {
		            .id          = CommandId::kVersion,
		            .name        = "version",
		            .summary     = "Print version",
		            .kind        = CommandKind::kVersion,
		            .user_target = UserTargetMode::kNone,
		            .completion  = CommandCompletionKind::kNone,
		        },
		    }};

		constexpr std::array<GlobalOptionDescriptor, 4> kGlobalOptionCatalog = {{
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

	auto global_option_catalog_is_valid() -> bool {
		const auto spellings_collide = [](std::string_view left, std::string_view right) -> bool {
			return !left.empty() && !right.empty() && left == right;
		};
		std::array<bool, static_cast<std::size_t>(GlobalOptionId::kCount)> seen_ids{};
		for (std::size_t index = 0; index < kGlobalOptionCatalog.size(); ++index) {
			const auto &option = kGlobalOptionCatalog[index];
			const auto  id     = static_cast<std::size_t>(option.id);
			if (id >= seen_ids.size() || seen_ids[id] ||
			    (option.short_name.empty() && option.long_name.empty()) || option.summary.empty() ||
			    (option.completion != GlobalOptionCompletionKind::kNone &&
			     option.argument_name.empty())) {
				return false;
			}
			seen_ids[id] = true;
			for (std::size_t previous = 0; previous < index; ++previous) {
				const auto &previous_option = kGlobalOptionCatalog[previous];
				if (spellings_collide(option.short_name, previous_option.short_name) ||
				    spellings_collide(option.short_name, previous_option.long_name) ||
				    spellings_collide(option.long_name, previous_option.short_name) ||
				    spellings_collide(option.long_name, previous_option.long_name)) {
					return false;
				}
			}
		}
		return std::ranges::all_of(seen_ids, [](bool seen) -> bool {
			return seen;
		});
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
