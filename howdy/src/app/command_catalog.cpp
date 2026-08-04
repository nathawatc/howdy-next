#include "app/command_catalog.hpp"

#include <algorithm>
#include <array>

namespace howdy::native {

	namespace {

		constexpr std::array<CommandDescriptor, 11> kCommandCatalog = {{
		    {
		        .id                    = CommandId::kAdd,
		        .name                  = "add",
		        .summary               = "Add face model",
		        .accepts_user_argument = true,
		    },
		    {
		        .id                    = CommandId::kClear,
		        .name                  = "clear",
		        .summary               = "Remove all models",
		        .accepts_user_argument = true,
		    },
		    {
		        .id                    = CommandId::kConfig,
		        .name                  = "config",
		        .summary               = "Edit config",
		        .accepts_user_argument = false,
		    },
		    {
		        .id                    = CommandId::kDisable,
		        .name                  = "disable",
		        .summary               = "Enable or disable auth",
		        .accepts_user_argument = false,
		    },
		    {
		        .id                    = CommandId::kDownloadModels,
		        .name                  = "download-models",
		        .summary               = "Download ONNX models",
		        .accepts_user_argument = false,
		    },
		    {
		        .id                    = CommandId::kList,
		        .name                  = "list",
		        .summary               = "List models",
		        .accepts_user_argument = true,
		    },
		    {
		        .id                    = CommandId::kRemove,
		        .name                  = "remove",
		        .summary               = "Remove a specific model",
		        .accepts_user_argument = true,
		    },
		    {
		        .id                    = CommandId::kSet,
		        .name                  = "set",
		        .summary               = "Edit config value",
		        .accepts_user_argument = false,
		    },
		    {
		        .id                    = CommandId::kSnapshot,
		        .name                  = "snapshot",
		        .summary               = "Camera preview",
		        .accepts_user_argument = false,
		    },
		    {
		        .id                    = CommandId::kTest,
		        .name                  = "test",
		        .summary               = "Test camera",
		        .accepts_user_argument = true,
		    },
		    {
		        .id                    = CommandId::kVersion,
		        .name                  = "version",
		        .summary               = "Print version",
		        .accepts_user_argument = false,
		    },
		}};

		constexpr std::array<GlobalOptionDescriptor, 4> kGlobalOptionCatalog = {{
		    {
		        .id            = GlobalOptionId::kUser,
		        .short_name    = "-U",
		        .long_name     = "--user",
		        .argument_name = "USER",
		        .summary       = "Target user for model commands",
		    },
		    {
		        .id            = GlobalOptionId::kPlain,
		        .short_name    = "",
		        .long_name     = "--plain",
		        .argument_name = "",
		        .summary       = "Disable interactive prompts where supported",
		    },
		    {
		        .id            = GlobalOptionId::kYes,
		        .short_name    = "-y",
		        .long_name     = "",
		        .argument_name = "",
		        .summary       = "Assume yes where supported",
		    },
		    {
		        .id            = GlobalOptionId::kHelp,
		        .short_name    = "-h",
		        .long_name     = "--help",
		        .argument_name = "",
		        .summary       = "Show this help",
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
			    (option.short_name.empty() && option.long_name.empty()) || option.summary.empty()) {
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
