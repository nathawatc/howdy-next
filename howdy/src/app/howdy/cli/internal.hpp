#pragma once

#include "app/command_catalog.hpp"
#include "app/command_invocation.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace howdy::native::howdy_cli_internal {

	enum class CliSyntaxErrorKind : std::uint8_t {
		kUnexpectedArgument,
		kUnrecognizedSubcommand,
		kMissingOptionValue,
		kEmptyOptionValue,
		kDuplicateOption,
		kMissingRequiredArguments,
	};

	struct CliSyntaxError {
		CliSyntaxErrorKind kind;
		std::string        value;
		std::string        argument_name;
		std::size_t        positional_count    = 0;
		bool               suggest_double_dash = false;
	};

	struct ParsedArgument {
		std::string value;
		bool        options_enabled = true;
	};

	struct ParsedCommandLine {
		std::optional<std::string>  command;
		std::optional<std::string>  user;
		std::string                 user_option_spelling;
		bool                        assume_yes         = false;
		bool                        plain              = false;
		bool                        global_option_seen = false;
		bool                        help_requested     = false;
		bool                        help_after_command = false;
		std::vector<ParsedArgument> arguments;
	};

	auto ParseCommandLine(int argc, char **argv, ParsedCommandLine &parsed)
	    -> std::optional<CliSyntaxError>;
	auto ParseCommandInvocation(const ParsedCommandLine &parsed, const CommandDescriptor &command)
	    -> std::expected<CommandInvocation, CliSyntaxError>;
	auto PrintCliSyntaxError(const CliSyntaxError &error, const CommandDescriptor *command) -> int;
	void PrintHelp();
	void PrintCommandHelp(const CommandDescriptor &command);

}  // namespace howdy::native::howdy_cli_internal
