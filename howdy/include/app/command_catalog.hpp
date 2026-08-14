#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
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
		kCount,
	};

	enum class CommandKind : std::uint8_t {
		kEntrypoint,
		kVersion,
	};

	enum class UserTargetMode : std::uint8_t {
		kNone,
		kModelUser,
	};

	enum class CommandCompletionKind : std::uint8_t {
		kNone,
		kBoolean,
		kConfigSet,
	};

	enum class GlobalOptionCompletionKind : std::uint8_t {
		kNone,
		kUser,
	};

	struct CommandDescriptor {
		CommandId             id;
		std::string_view      name;
		std::string_view      summary;
		CommandKind           kind;
		UserTargetMode        user_target;
		CommandCompletionKind completion;
		std::string_view      argument_synopsis;
	};

	enum class GlobalOptionId : std::uint8_t {
		kUser,
		kPlain,
		kYes,
		kHelp,
		kCount,
	};

	struct GlobalOptionDescriptor {
		GlobalOptionId             id;
		std::string_view           short_name;
		std::string_view           long_name;
		std::string_view           argument_name;
		std::string_view           summary;
		GlobalOptionCompletionKind completion;
		bool                       parses_after_command;
	};

	[[nodiscard]] auto command_catalog() -> std::span<const CommandDescriptor>;
	[[nodiscard]] auto validate_command_catalog(std::span<const CommandDescriptor> commands,
	                                            bool require_complete = false)
	    -> std::optional<std::string>;
	[[nodiscard]] auto global_option_catalog() -> std::span<const GlobalOptionDescriptor>;
	[[nodiscard]] auto
	validate_global_option_catalog(std::span<const GlobalOptionDescriptor> options,
	                               bool require_complete = false) -> std::optional<std::string>;
	[[nodiscard]] auto find_command(std::string_view name) -> const CommandDescriptor *;
	[[nodiscard]] auto find_global_option(GlobalOptionId id) -> const GlobalOptionDescriptor *;
	[[nodiscard]] auto find_global_option(std::string_view spelling)
	    -> const GlobalOptionDescriptor *;

}  // namespace howdy::native
