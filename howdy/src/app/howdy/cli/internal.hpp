#pragma once

#include "app/command_catalog.hpp"

#include <cstddef>
#include <cstdint>
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
		bool                        yes                = false;
		bool                        plain              = false;
		bool                        global_option_seen = false;
		bool                        help_requested     = false;
		bool                        help_after_command = false;
		std::vector<ParsedArgument> arguments;
	};

	auto parse_command_line(int argc, char **argv, ParsedCommandLine &parsed)
	    -> std::optional<CliSyntaxError>;
	auto command_syntax_error(const ParsedCommandLine &parsed, const CommandDescriptor &command)
	    -> std::optional<CliSyntaxError>;
	auto print_cli_syntax_error(const CliSyntaxError &error, const CommandDescriptor *command)
	    -> int;
	void print_help();
	void print_command_help(const CommandDescriptor &command);

}  // namespace howdy::native::howdy_cli_internal
