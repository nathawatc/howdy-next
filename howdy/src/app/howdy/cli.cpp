#include "app/command_catalog.hpp"
#include "cli/internal.hpp"

#include <algorithm>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace howdy::native::howdy_cli_internal {
	namespace {

		template <typename OptionDescriptor>
		auto FormatOptionLabel(const OptionDescriptor &option) -> std::string {
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
			if (!option.argument_name.empty()) {
				label += " <";
				label += option.argument_name;
				label += '>';
			}
			return label;
		}

		auto FormatCommandUsage(const CommandDescriptor &command) -> std::string {
			std::string usage = "howdy ";
			usage += command.name;
			usage += " [OPTIONS]";
			if (command.max_positionals > 0 && !command.argument_synopsis.empty()) {
				usage += ' ';
				usage += command.argument_synopsis;
			}
			return usage;
		}

		void PrintUsageFooter(const CommandDescriptor *command) {
			std::cerr << "\nUsage: ";
			if (command == nullptr) {
				std::cerr << "howdy [OPTIONS] <COMMAND>\n";
			} else {
				std::cerr << FormatCommandUsage(*command) << '\n';
			}
			std::cerr << "\nFor more information, try '--help'.\n";
		}

		auto OptionWithArgument(const CliSyntaxError &error) -> std::string {
			std::string label(error.value);
			if (!error.argument_name.empty()) {
				label += " <";
				label += error.argument_name;
				label += '>';
			}
			return label;
		}

		void PrintMissingRequiredArguments(const CommandDescriptor &command,
		                                   std::size_t              positional_count) {
			std::istringstream synopsis{std::string(command.argument_synopsis)};
			std::string        token;
			std::size_t        index = 0;
			while (synopsis >> token) {
				if (!token.empty() && token.front() == '[') {
					continue;
				}
				if (index++ < positional_count) {
					continue;
				}
				std::cerr << "  " << token << '\n';
			}
		}

		auto CanParseGlobalOption(const ParsedCommandLine      &parsed,
		                          const GlobalOptionDescriptor &option) -> bool {
			if (!parsed.command.has_value()) {
				return true;
			}
			const bool known_command =
			    FindCommand(*parsed.command) != nullptr || *parsed.command == "__complete";
			return known_command &&
			       (option.parses_after_command || option.id == GlobalOptionId::kHelp);
		}

		auto ParseGlobalOption(const GlobalOptionDescriptor &option, std::string_view spelling,
		                       int argc, char **argv, int &index, ParsedCommandLine &parsed)
		    -> std::optional<CliSyntaxError> {
			switch (option.id) {
				case GlobalOptionId::kUser:
					parsed.global_option_seen = true;
					if (index + 1 >= argc) {
						return CliSyntaxError{
						    .kind          = CliSyntaxErrorKind::kMissingOptionValue,
						    .value         = std::string(spelling),
						    .argument_name = std::string(option.argument_name),
						};
					}
					parsed.user_option_spelling = spelling;
					parsed.user                 = argv[++index];
					if (parsed.user->empty()) {
						return CliSyntaxError{
						    .kind          = CliSyntaxErrorKind::kEmptyOptionValue,
						    .value         = parsed.user_option_spelling,
						    .argument_name = std::string(option.argument_name),
						};
					}
					break;
				case GlobalOptionId::kYes:
					parsed.global_option_seen = true;
					parsed.assume_yes         = true;
					break;
				case GlobalOptionId::kPlain:
					parsed.global_option_seen = true;
					parsed.plain              = true;
					break;
				case GlobalOptionId::kHelp:
					parsed.help_requested     = true;
					parsed.help_after_command = parsed.command.has_value();
					break;
				case GlobalOptionId::kCount:
					break;
			}
			return std::nullopt;
		}

		auto GlobalOptionError(const ParsedCommandLine &parsed, const CommandDescriptor &command)
		    -> std::optional<CliSyntaxError> {
			if (parsed.user.has_value() &&
			    !CommandAcceptsGlobalOption(command, GlobalOptionId::kUser)) {
				return CliSyntaxError{
				    .kind  = CliSyntaxErrorKind::kUnexpectedArgument,
				    .value = parsed.user_option_spelling,
				};
			}
			if (parsed.plain && !CommandAcceptsGlobalOption(command, GlobalOptionId::kPlain)) {
				return CliSyntaxError{
				    .kind  = CliSyntaxErrorKind::kUnexpectedArgument,
				    .value = "--plain",
				};
			}
			if (parsed.assume_yes && !CommandAcceptsGlobalOption(command, GlobalOptionId::kYes)) {
				return CliSyntaxError{
				    .kind  = CliSyntaxErrorKind::kUnexpectedArgument,
				    .value = "-y",
				};
			}
			return std::nullopt;
		}

		auto AppendPositional(CommandInvocation &invocation, const CommandDescriptor &command,
		                      std::string_view value, std::size_t &positional_count)
		    -> std::optional<CliSyntaxError> {
			if (positional_count >= command.max_positionals) {
				return CliSyntaxError{
				    .kind  = CliSyntaxErrorKind::kUnexpectedArgument,
				    .value = std::string(value),
				};
			}
			invocation.positionals.emplace_back(value);
			++positional_count;
			return std::nullopt;
		}

		auto ParseCommandOption(const ParsedCommandLine &parsed, const CommandDescriptor &command,
		                        const CommandOptionDescriptor &option, std::size_t &index,
		                        std::vector<const CommandOptionDescriptor *> &seen_options,
		                        CommandInvocation &invocation) -> std::optional<CliSyntaxError> {
			const auto &argument = parsed.arguments[index];
			if (std::ranges::find(seen_options, &option) != seen_options.end()) {
				return CliSyntaxError{
				    .kind  = CliSyntaxErrorKind::kDuplicateOption,
				    .value = argument.value,
				};
			}
			seen_options.push_back(&option);
			if (option.argument_name.empty()) {
				return std::nullopt;
			}

			const std::size_t value_index = index + 1;
			if (value_index >= parsed.arguments.size() ||
			    !parsed.arguments[value_index].options_enabled ||
			    (!parsed.arguments[value_index].value.empty() &&
			     parsed.arguments[value_index].value.front() == '-')) {
				return CliSyntaxError{
				    .kind          = CliSyntaxErrorKind::kMissingOptionValue,
				    .value         = argument.value,
				    .argument_name = std::string(option.argument_name),
				};
			}
			if (parsed.arguments[value_index].value.empty()) {
				return CliSyntaxError{
				    .kind          = CliSyntaxErrorKind::kEmptyOptionValue,
				    .value         = argument.value,
				    .argument_name = std::string(option.argument_name),
				};
			}
			if (command.id == CommandId::kTest && option.long_name == "--device") {
				invocation.device = parsed.arguments[value_index].value;
			}
			index = value_index;
			return std::nullopt;
		}

	}  // namespace

	void PrintHelp() {
		std::cout << "Usage: howdy [OPTIONS] <COMMAND>\n\n";
		std::cout << "Commands:\n";
		for (const auto &descriptor : CommandCatalog()) {
			std::cout << "  " << std::left << std::setw(17) << descriptor.name << descriptor.summary
			          << '\n';
		}
		std::cout << "\nOptions:\n";
		for (const auto &option : GlobalOptionCatalog()) {
			std::cout << "  " << std::left << std::setw(22) << FormatOptionLabel(option)
			          << option.summary << '\n';
		}
	}

	void PrintCommandHelp(const CommandDescriptor &command) {
		std::cout << command.summary << "\n\n";
		std::cout << "Usage: " << FormatCommandUsage(command) << "\n\n";
		std::cout << "Options:\n";
		for (const auto &option : GlobalOptionCatalog()) {
			if (option.id == GlobalOptionId::kHelp ||
			    CommandAcceptsGlobalOption(command, option.id)) {
				std::cout << "  " << std::left << std::setw(22) << FormatOptionLabel(option)
				          << option.summary << '\n';
			}
		}
		for (const auto &option : command.options) {
			std::cout << "  " << std::left << std::setw(22) << FormatOptionLabel(option)
			          << option.summary << '\n';
		}
	}

	auto PrintCliSyntaxError(const CliSyntaxError &error, const CommandDescriptor *command) -> int {
		switch (error.kind) {
			case CliSyntaxErrorKind::kUnexpectedArgument:
				std::cerr << "error: unexpected argument '" << error.value << "' found\n";
				if (error.suggest_double_dash) {
					std::cerr << "\n  tip: to pass '" << error.value << "' as a value, use '-- "
					          << error.value << "'\n";
				}
				break;
			case CliSyntaxErrorKind::kUnrecognizedSubcommand:
				std::cerr << "error: unrecognized subcommand '" << error.value << "'\n";
				break;
			case CliSyntaxErrorKind::kMissingOptionValue:
				std::cerr << "error: a value is required for '" << OptionWithArgument(error)
				          << "' but none was supplied\n";
				break;
			case CliSyntaxErrorKind::kEmptyOptionValue:
				std::cerr << "error: invalid value '' for '" << OptionWithArgument(error)
				          << "': value cannot be empty\n";
				break;
			case CliSyntaxErrorKind::kDuplicateOption:
				std::cerr << "error: the argument '" << error.value
				          << "' cannot be used multiple times\n";
				break;
			case CliSyntaxErrorKind::kMissingRequiredArguments:
				std::cerr << "error: the following required arguments were not provided:\n";
				if (command != nullptr) {
					PrintMissingRequiredArguments(*command, error.positional_count);
				}
				break;
		}
		PrintUsageFooter(command);
		return 2;
	}

	auto ParseCommandLine(int argc, char **argv, ParsedCommandLine &parsed)
	    -> std::optional<CliSyntaxError> {
		bool options_ended = false;
		for (int index = 1; index < argc; ++index) {
			const std::string_view arg(argv[index]);
			if (!options_ended && arg == "--") {
				options_ended = true;
				continue;
			}
			if (!options_ended) {
				const auto *option = FindGlobalOption(arg);
				if (option != nullptr && CanParseGlobalOption(parsed, *option)) {
					if (const auto error =
					        ParseGlobalOption(*option, arg, argc, argv, index, parsed);
					    error.has_value()) {
						return error;
					}
					continue;
				}
			}
			if (!parsed.command.has_value()) {
				if (!options_ended && arg.size() > 1 && arg.front() == '-') {
					return CliSyntaxError{
					    .kind  = CliSyntaxErrorKind::kUnexpectedArgument,
					    .value = std::string(arg),
					};
				}
				parsed.command = std::string(arg);
				continue;
			}
			parsed.arguments.push_back(
			    {.value = std::string(arg), .options_enabled = !options_ended});
		}
		return std::nullopt;
	}

	auto ParseCommandInvocation(const ParsedCommandLine &parsed, const CommandDescriptor &command)
	    -> std::expected<CommandInvocation, CliSyntaxError> {
		if (const auto error = GlobalOptionError(parsed, command); error.has_value()) {
			return std::unexpected(*error);
		}

		CommandInvocation invocation{
		    .resolved_user =
		        command.user_target == UserTargetMode::kModelUser ? parsed.user : std::nullopt,
		    .plain      = parsed.plain,
		    .assume_yes = parsed.assume_yes,
		};
		std::size_t                                  positional_count = 0;
		std::vector<const CommandOptionDescriptor *> seen_options;
		for (std::size_t index = 0; index < parsed.arguments.size(); ++index) {
			const auto &argument = parsed.arguments[index];
			if (!argument.options_enabled) {
				if (const auto error =
				        AppendPositional(invocation, command, argument.value, positional_count);
				    error.has_value()) {
					return std::unexpected(*error);
				}
				continue;
			}

			const auto *option = FindCommandOption(command, argument.value);
			if (option != nullptr) {
				if (const auto error = ParseCommandOption(parsed, command, *option, index,
				                                          seen_options, invocation);
				    error.has_value()) {
					return std::unexpected(*error);
				}
				continue;
			}
			if (!argument.value.empty() && argument.value.front() == '-') {
				return std::unexpected(CliSyntaxError{
				    .kind                = CliSyntaxErrorKind::kUnexpectedArgument,
				    .value               = argument.value,
				    .suggest_double_dash = positional_count < command.max_positionals,
				});
			}
			if (const auto error =
			        AppendPositional(invocation, command, argument.value, positional_count);
			    error.has_value()) {
				return std::unexpected(*error);
			}
		}

		if (positional_count < command.min_positionals) {
			return std::unexpected(CliSyntaxError{
			    .kind             = CliSyntaxErrorKind::kMissingRequiredArguments,
			    .positional_count = positional_count,
			});
		}
		return invocation;
	}

}  // namespace howdy::native::howdy_cli_internal
