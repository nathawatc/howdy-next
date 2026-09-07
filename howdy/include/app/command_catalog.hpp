#pragma once

#include <cstddef>
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

	enum class GlobalOptionId : std::uint8_t {
		kUser,
		kPlain,
		kYes,
		kHelp,
		kCount,
	};

	using GlobalOptionMask = std::uint8_t;

	constexpr auto GlobalOptionBit(GlobalOptionId id) -> GlobalOptionMask {
		if (id == GlobalOptionId::kCount) {
			return 0;
		}
		return static_cast<GlobalOptionMask>(1U << static_cast<unsigned int>(id));
	}

	enum class GlobalOptionCompletionKind : std::uint8_t {
		kNone,
		kUser,
	};

	struct CommandOptionDescriptor {
		std::string_view short_name;
		std::string_view long_name;
		std::string_view argument_name;
		std::string_view summary;
	};

	struct CommandDescriptor {
		CommandId                                id;
		std::string_view                         name;
		std::string_view                         summary;
		CommandKind                              kind;
		UserTargetMode                           user_target;
		CommandCompletionKind                    completion;
		std::string_view                         argument_synopsis;
		std::size_t                              min_positionals = 0;
		std::size_t                              max_positionals = 0;
		GlobalOptionMask                         global_options  = 0;
		std::span<const CommandOptionDescriptor> options;
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

	[[nodiscard]] auto CommandCatalog() -> std::span<const CommandDescriptor>;
	[[nodiscard]] auto ValidateCommandCatalog(std::span<const CommandDescriptor> commands,
	                                          bool require_complete = false)
	    -> std::optional<std::string>;
	[[nodiscard]] auto GlobalOptionCatalog() -> std::span<const GlobalOptionDescriptor>;
	[[nodiscard]] auto ValidateGlobalOptionCatalog(std::span<const GlobalOptionDescriptor> options,
	                                               bool require_complete = false)
	    -> std::optional<std::string>;
	[[nodiscard]] auto FindCommand(std::string_view name) -> const CommandDescriptor *;
	[[nodiscard]] auto CommandAcceptsGlobalOption(const CommandDescriptor &command,
	                                              GlobalOptionId           id) -> bool;
	[[nodiscard]] auto FindCommandOption(const CommandDescriptor &command,
	                                     std::string_view         spelling)
	    -> const CommandOptionDescriptor *;
	[[nodiscard]] auto FindGlobalOption(GlobalOptionId id) -> const GlobalOptionDescriptor *;
	[[nodiscard]] auto FindGlobalOption(std::string_view spelling)
	    -> const GlobalOptionDescriptor *;

}  // namespace howdy::native
