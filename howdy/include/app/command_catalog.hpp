#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace howdy::native {

	enum class CommandId : std::uint8_t {
		kAdd,
		kClear,
		kConfig,
		kDisable,
		kDownloadModels,
		kList,
		kRemove,
		kSet,
		kSnapshot,
		kTest,
		kVersion,
	};

	struct CommandDescriptor {
		CommandId        id;
		std::string_view name;
		std::string_view summary;
		bool             accepts_user_argument;
	};

	enum class GlobalOptionId : std::uint8_t {
		kUser,
		kPlain,
		kYes,
		kHelp,
		kCount,
	};

	struct GlobalOptionDescriptor {
		GlobalOptionId   id;
		std::string_view short_name;
		std::string_view long_name;
		std::string_view argument_name;
		std::string_view summary;
	};

	[[nodiscard]] auto command_catalog() -> std::span<const CommandDescriptor>;
	[[nodiscard]] auto global_option_catalog() -> std::span<const GlobalOptionDescriptor>;
	[[nodiscard]] auto global_option_catalog_is_valid() -> bool;
	[[nodiscard]] auto find_command(std::string_view name) -> const CommandDescriptor *;
	[[nodiscard]] auto find_global_option(GlobalOptionId id) -> const GlobalOptionDescriptor *;
	[[nodiscard]] auto find_global_option(std::string_view spelling)
	    -> const GlobalOptionDescriptor *;

}  // namespace howdy::native
