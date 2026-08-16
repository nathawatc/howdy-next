#include "app/command_catalog.hpp"
#include "app/howdy_cli.hpp"
#include "app/howdy_internal.hpp"
#include "cli/add_cli.hpp"
#include "cli/clear_cli.hpp"
#include "cli/config_cli.hpp"
#include "cli/disable_cli.hpp"
#include "cli/download_models_cli.hpp"
#include "cli/list_cli.hpp"
#include "cli/remove_cli.hpp"
#include "cli/set_cli.hpp"
#include "cli/snapshot_cli.hpp"
#include "cli/test_cli.hpp"
#include "config/config_schema.hpp"
#include "howdy/version_format.hpp"
#include "support/user_names.hpp"
#include "version.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <pwd.h>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

	using howdy::native::howdy_internal::CommandMain;
	using howdy::native::howdy_internal::HowdyDependencies;

	constexpr std::array<std::string_view, 2> kBooleanCompletionValues{"false", "true"};

	auto print_completion_commands(std::span<const std::string> required_options = {}) -> bool {
		std::vector<howdy::native::GlobalOptionId> required_option_ids;
		required_option_ids.reserve(required_options.size());
		for (const auto &spelling : required_options) {
			const auto *option = howdy::native::find_global_option(spelling);
			if (option == nullptr || !option->parses_after_command) {
				return false;
			}
			required_option_ids.push_back(option->id);
		}

		for (const auto &descriptor : howdy::native::command_catalog()) {
			if (std::ranges::all_of(required_option_ids, [&](const auto id) -> auto {
				    return howdy::native::command_accepts_global_option(descriptor, id);
			    })) {
				std::cout << descriptor.name << '\n';
			}
		}
		return true;
	}

	auto completion_kind_name(howdy::native::GlobalOptionCompletionKind kind) -> std::string_view {
		switch (kind) {
			case howdy::native::GlobalOptionCompletionKind::kNone:
				return "none";
			case howdy::native::GlobalOptionCompletionKind::kUser:
				return "user";
		}
		return "none";
	}

	void print_completion_global_options() {
		for (const auto &option : howdy::native::global_option_catalog()) {
			for (const auto spelling : {option.short_name, option.long_name}) {
				if (!spelling.empty()) {
					std::cout << spelling << '\t' << (!option.argument_name.empty() ? '1' : '0')
					          << '\t' << (option.parses_after_command ? '1' : '0') << '\t'
					          << completion_kind_name(option.completion) << '\n';
				}
			}
		}
	}

	void print_completion_command_options(std::string_view command_name) {
		const auto *descriptor = howdy::native::find_command(command_name);
		if (descriptor == nullptr) {
			return;
		}
		for (const auto &option : howdy::native::global_option_catalog()) {
			if (!option.parses_after_command ||
			    !howdy::native::command_accepts_global_option(*descriptor, option.id)) {
				continue;
			}
			for (const auto spelling : {option.short_name, option.long_name}) {
				if (!spelling.empty()) {
					std::cout << spelling << '\t' << (!option.argument_name.empty() ? '1' : '0')
					          << "\tnone\n";
				}
			}
		}
		for (const auto &option : descriptor->options) {
			for (const auto spelling : {option.short_name, option.long_name}) {
				if (!spelling.empty()) {
					std::cout << spelling << '\t' << (!option.argument_name.empty() ? '1' : '0')
					          << "\tnone\n";
				}
			}
		}
	}

	auto parse_completion_position(std::string_view text, std::size_t &position) -> bool {
		if (text.empty()) {
			return false;
		}
		const auto result = std::from_chars(text.data(), text.data() + text.size(), position);
		return result.ec == std::errc{} && result.ptr == text.data() + text.size();
	}

	void print_config_option_values(const howdy::native::config_schema::Option &option) {
		if (!option.choices.empty()) {
			for (const auto choice : option.choices) {
				std::cout << choice << '\n';
			}
			return;
		}
		switch (option.type) {
			case howdy::native::config_schema::ValueType::boolean:
				for (const auto value : kBooleanCompletionValues) {
					std::cout << value << '\n';
				}
				return;
			case howdy::native::config_schema::ValueType::integer:
			case howdy::native::config_schema::ValueType::floating_point:
			case howdy::native::config_schema::ValueType::string:
				return;
		}
	}

	auto print_completion_command_values(std::string_view command_name, std::size_t position,
	                                     std::span<const std::string> previous_positionals)
	    -> bool {
		const auto *descriptor = howdy::native::find_command(command_name);
		if (descriptor == nullptr || previous_positionals.size() != position) {
			return false;
		}
		if (descriptor->completion == howdy::native::CommandCompletionKind::kBoolean) {
			if (position != 0) {
				return false;
			}
			for (const auto value : kBooleanCompletionValues) {
				std::cout << value << '\n';
			}
			return true;
		}
		if (descriptor->completion != howdy::native::CommandCompletionKind::kConfigSet) {
			return position == 0;
		}
		if (position == 0) {
			for (const auto &option : howdy::native::config_schema::runtime_config_options()) {
				std::cout << option.key << '\n';
			}
			return true;
		}
		if (position != 1) {
			return false;
		}
		const auto &key = previous_positionals.front();
		for (const auto &option : howdy::native::config_schema::runtime_config_options()) {
			if (option.key == key) {
				print_config_option_values(option);
				break;
			}
		}
		return true;
	}

	auto handle_completion_query(const std::vector<std::string> &arguments, bool global_option_seen)
	    -> int {
		if (global_option_seen) {
			return 1;
		}
		if (!arguments.empty() && arguments.front() == "commands") {
			return print_completion_commands(std::span<const std::string>(arguments).subspan(1))
			           ? 0
			           : 1;
		}
		if (arguments.size() == 1 && arguments.front() == "global-options") {
			print_completion_global_options();
			return 0;
		}
		if (arguments.size() == 2 && arguments.front() == "command-options") {
			print_completion_command_options(arguments[1]);
			return howdy::native::find_command(arguments[1]) == nullptr ? 1 : 0;
		}
		if (arguments.size() >= 3 && arguments.front() == "command-values") {
			std::size_t position = 0;
			if (parse_completion_position(arguments[2], position) &&
			    position == arguments.size() - 3 &&
			    print_completion_command_values(
			        arguments[1], position, std::span<const std::string>(arguments).subspan(3))) {
				return 0;
			}
		}
		return 1;
	}

	auto format_option_label(const howdy::native::GlobalOptionDescriptor &option) -> std::string {
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
			label += ' ';
			label += option.argument_name;
		}
		return label;
	}

	auto resolve_user(void *context) -> std::string {
		(void)context;
		for (const char *name : {"SUDO_USER", "DOAS_USER"}) {
			if (const char *value = std::getenv(name); value != nullptr && value[0] != '\0') {
				return value;
			}
		}

		if (const char *pkexec_uid = std::getenv("PKEXEC_UID");
		    pkexec_uid != nullptr && pkexec_uid[0] != '\0') {
			errno              = 0;
			char      *end     = nullptr;
			const auto raw_uid = std::strtoul(pkexec_uid, &end, 10);
			if (errno == 0 && end != pkexec_uid && end != nullptr && *end == '\0' &&
			    raw_uid <= std::numeric_limits<uid_t>::max()) {
				const auto uid = static_cast<uid_t>(raw_uid);
				if (passwd *pwd = getpwuid(uid); pwd != nullptr) {
					return {pwd->pw_name};
				}
			}
		}

		if (passwd *pwd = getpwuid(getuid()); pwd != nullptr) {
			return pwd->pw_name;
		}
		return {};
	}

	auto effective_uid(void *context) -> uid_t {
		(void)context;
		return geteuid();
	}

	auto production_command_mains()
	    -> std::array<CommandMain, static_cast<std::size_t>(howdy::native::CommandId::kCount)> {
		std::array<CommandMain, static_cast<std::size_t>(howdy::native::CommandId::kCount)>
		    command_mains{};
		command_mains[static_cast<std::size_t>(howdy::native::CommandId::kAdd)]     = add_main;
		command_mains[static_cast<std::size_t>(howdy::native::CommandId::kClear)]   = clear_main;
		command_mains[static_cast<std::size_t>(howdy::native::CommandId::kConfig)]  = config_main;
		command_mains[static_cast<std::size_t>(howdy::native::CommandId::kDisable)] = disable_main;
		command_mains[static_cast<std::size_t>(howdy::native::CommandId::kDownloadModels)] =
		    download_models_main;
		command_mains[static_cast<std::size_t>(howdy::native::CommandId::kList)]   = list_main;
		command_mains[static_cast<std::size_t>(howdy::native::CommandId::kRemove)] = remove_main;
		command_mains[static_cast<std::size_t>(howdy::native::CommandId::kSet)]    = set_main;
		command_mains[static_cast<std::size_t>(howdy::native::CommandId::kSnapshot)] =
		    snapshot_main;
		command_mains[static_cast<std::size_t>(howdy::native::CommandId::kTest)] = test_main;
		return command_mains;
	}

	auto format_usage_option(const howdy::native::GlobalOptionDescriptor &option) -> std::string {
		std::string label = option.short_name.empty() ? std::string(option.long_name)
		                                              : std::string(option.short_name);
		if (!option.argument_name.empty()) {
			label += ' ';
			label += option.argument_name;
		}
		return "[" + label + "]";
	}

	void print_help() {
		std::cout << "usage: howdy "
		          << howdy::native::howdy_internal::format_usage_options(
		                 howdy::native::global_option_catalog())
		          << " {command} [arguments...]\n\n";
		std::cout << "commands:\n";
		for (const auto &descriptor : howdy::native::command_catalog()) {
			std::cout << "  " << std::left << std::setw(17) << descriptor.name << descriptor.summary
			          << '\n';
		}
		std::cout << "\noptions:\n";
		for (const auto &option : howdy::native::global_option_catalog()) {
			std::cout << "  " << std::left << std::setw(17) << format_option_label(option)
			          << option.summary << '\n';
		}
	}

	struct ParsedArgument {
		std::string value;
		bool        options_enabled = true;
	};

	struct ParsedCommandLine {
		std::optional<std::string>  command;
		std::optional<std::string>  user;
		bool                        yes                = false;
		bool                        plain              = false;
		bool                        global_option_seen = false;
		bool                        help_requested     = false;
		std::vector<ParsedArgument> arguments;
	};

	auto parse_command_line(int argc, char **argv, ParsedCommandLine &parsed)
	    -> std::optional<int> {
		bool options_ended = false;
		for (int index = 1; index < argc; ++index) {
			const std::string_view arg(argv[index]);
			if (!options_ended && arg == "--") {
				options_ended = true;
				continue;
			}
			if (!options_ended) {
				if (const auto *option = howdy::native::find_global_option(arg);
				    option != nullptr &&
				    (!parsed.command.has_value() || option->parses_after_command)) {
					switch (option->id) {
						case howdy::native::GlobalOptionId::kUser:
							parsed.global_option_seen = true;
							if (index + 1 >= argc) {
								std::cout << "Option '" << arg << "' requires an argument\n";
								return 1;
							}
							parsed.user = argv[++index];
							continue;
						case howdy::native::GlobalOptionId::kYes:
							parsed.global_option_seen = true;
							parsed.yes                = true;
							continue;
						case howdy::native::GlobalOptionId::kPlain:
							parsed.global_option_seen = true;
							parsed.plain              = true;
							continue;
						case howdy::native::GlobalOptionId::kHelp:
							parsed.help_requested = true;
							continue;
						case howdy::native::GlobalOptionId::kCount:
							break;
					}
				}
			}
			if (!parsed.command.has_value()) {
				parsed.command = std::string(arg);
				continue;
			}
			parsed.arguments.push_back(
			    {.value = std::string(arg), .options_enabled = !options_ended});
		}
		return std::nullopt;
	}

	auto global_option_syntax_error(const ParsedCommandLine                &parsed,
	                                const howdy::native::CommandDescriptor &command)
	    -> std::optional<std::string> {
		if (parsed.user.has_value() && !howdy::native::command_accepts_global_option(
		                                   command, howdy::native::GlobalOptionId::kUser)) {
			return "option '--user' is not valid for this command";
		}
		if (parsed.plain && !howdy::native::command_accepts_global_option(
		                        command, howdy::native::GlobalOptionId::kPlain)) {
			return "option '--plain' is not valid for this command";
		}
		if (parsed.yes && !howdy::native::command_accepts_global_option(
		                      command, howdy::native::GlobalOptionId::kYes)) {
			return "option '-y' is not valid for this command";
		}
		return std::nullopt;
	}

	auto command_argument_syntax_error(const ParsedCommandLine                &parsed,
	                                   const howdy::native::CommandDescriptor &command)
	    -> std::optional<std::string> {
		std::size_t                                                 positional_count = 0;
		std::vector<const howdy::native::CommandOptionDescriptor *> seen_options;
		for (std::size_t index = 0; index < parsed.arguments.size(); ++index) {
			const auto &argument = parsed.arguments[index];
			if (!argument.options_enabled) {
				++positional_count;
				continue;
			}
			const auto *option = howdy::native::find_command_option(command, argument.value);
			if (option != nullptr) {
				if (std::ranges::find(seen_options, option) != seen_options.end()) {
					return "option '" + argument.value + "' may be specified only once";
				}
				seen_options.push_back(option);
				if (option->argument_name.empty()) {
					continue;
				}
				if (index + 1 >= parsed.arguments.size() ||
				    !parsed.arguments[index + 1].options_enabled ||
				    parsed.arguments[index + 1].value.empty() ||
				    parsed.arguments[index + 1].value.front() == '-') {
					return "option '" + argument.value + "' requires a non-empty value";
				}
				++index;
				continue;
			}
			if (!argument.value.empty() && argument.value.front() == '-') {
				return "unknown option '" + argument.value + "'";
			}
			++positional_count;
		}

		if (positional_count < command.min_positionals ||
		    positional_count > command.max_positionals) {
			return "expected between " + std::to_string(command.min_positionals) + " and " +
			       std::to_string(command.max_positionals) + " positional argument(s)";
		}
		return std::nullopt;
	}

	auto command_syntax_error(const ParsedCommandLine                &parsed,
	                          const howdy::native::CommandDescriptor &command)
	    -> std::optional<std::string> {
		if (const auto error = global_option_syntax_error(parsed, command); error.has_value()) {
			return error;
		}
		return command_argument_syntax_error(parsed, command);
	}

	auto print_command_syntax_error(std::string_view command, std::string_view error) -> int {
		std::cout << "Invalid arguments for command '" << command << "': " << error << '\n';
		return 1;
	}

	auto reject_empty_user(const ParsedCommandLine &parsed) -> std::optional<int> {
		if (!parsed.user.has_value() || !parsed.user->empty()) {
			return std::nullopt;
		}
		std::cout << "Option '--user' requires a non-empty argument\n";
		return 1;
	}

	auto handle_special_command(const ParsedCommandLine &parsed) -> std::optional<int> {
		if (!parsed.command.has_value()) {
			if (const auto result = reject_empty_user(parsed); result.has_value()) {
				return result;
			}
			print_help();
			return 0;
		}
		if (parsed.help_requested) {
			if (const auto result = reject_empty_user(parsed); result.has_value()) {
				return result;
			}
			print_help();
			return 0;
		}
		if (*parsed.command != "__complete") {
			return std::nullopt;
		}
		if (const auto result = reject_empty_user(parsed); result.has_value()) {
			return result;
		}
		std::vector<std::string> completion_arguments;
		completion_arguments.reserve(parsed.arguments.size());
		for (const auto &argument : parsed.arguments) {
			completion_arguments.push_back(argument.value);
		}
		return handle_completion_query(completion_arguments, parsed.global_option_seen);
	}

	auto resolve_model_user(ParsedCommandLine &parsed, const HowdyDependencies &dependencies)
	    -> bool {
		if (!parsed.user.has_value()) {
			if (dependencies.resolve_user == nullptr) {
				return false;
			}
			parsed.user = dependencies.resolve_user(dependencies.context);
		}
		if (!parsed.user.has_value() || parsed.user->empty()) {
			std::cout << "Unable to determine the user; please use --user\n";
			return false;
		}
		if (!howdy::native::is_valid_model_user_name(*parsed.user)) {
			std::cout << howdy::native::kInvalidUserNameMessage << "\n";
			return false;
		}
		return true;
	}

	auto build_command_argv_strings(const howdy::native::CommandDescriptor &command,
	                                const ParsedCommandLine &parsed, std::string_view user)
	    -> std::vector<std::string> {
		std::vector<std::string> argv_strings;
		argv_strings.push_back("howdy-" + std::string(command.name));
		if (command.user_target == howdy::native::UserTargetMode::kModelUser) {
			argv_strings.emplace_back(user);
		}
		for (const auto &argument : parsed.arguments) {
			if (argument.options_enabled) {
				argv_strings.push_back(argument.value);
			}
		}
		if (parsed.plain) {
			argv_strings.emplace_back("--plain");
		}
		if (parsed.yes) {
			argv_strings.emplace_back("-y");
		}
		bool end_options_forwarded = false;
		for (const auto &argument : parsed.arguments) {
			if (argument.options_enabled) {
				continue;
			}
			if (!end_options_forwarded) {
				argv_strings.emplace_back("--");
				end_options_forwarded = true;
			}
			argv_strings.push_back(argument.value);
		}
		return argv_strings;
	}

}  // namespace

auto howdy::native::howdy_internal::format_usage_options(
    std::span<const howdy::native::GlobalOptionDescriptor> options) -> std::string {
	std::string rendered;
	// Preserve existing synopsis order generically: long-spelled options first, short-only last.
	for (const bool include_long_options : {true, false}) {
		for (const auto &option : options) {
			if ((!option.long_name.empty()) != include_long_options) {
				continue;
			}
			if (!rendered.empty()) {
				rendered += ' ';
			}
			rendered += format_usage_option(option);
		}
	}
	return rendered;
}

auto howdy::native::howdy_internal::howdy_main_with_dependencies(
    int argc, char **argv, const HowdyDependencies &dependencies) -> int {
	if (const auto error =
	        howdy::native::validate_command_catalog(howdy::native::command_catalog(), true)) {
		std::cerr << "howdy: command catalog validation failed: " << *error << '\n';
		return 1;
	}
	if (const auto error = howdy::native::validate_global_option_catalog(
	        howdy::native::global_option_catalog(), true)) {
		std::cerr << "howdy: global option catalog validation failed: " << *error << '\n';
		return 1;
	}

	ParsedCommandLine parsed;
	if (const auto parse_result = parse_command_line(argc, argv, parsed);
	    parse_result.has_value()) {
		return *parse_result;
	}

	if (const auto special_result = handle_special_command(parsed); special_result.has_value()) {
		return *special_result;
	}
	if (!parsed.command.has_value()) {
		return 1;
	}
	const auto &command_name       = parsed.command.value();
	const auto *command_descriptor = howdy::native::find_command(command_name);
	if (command_descriptor == nullptr) {
		std::cout << "Unknown command: " << command_name << "\n";
		return 1;
	}
	if (const auto error = command_syntax_error(parsed, *command_descriptor); error.has_value()) {
		return print_command_syntax_error(command_name, *error);
	}
	if (const auto empty_user_result = reject_empty_user(parsed); empty_user_result.has_value()) {
		return *empty_user_result;
	}
	if (command_descriptor->kind == howdy::native::CommandKind::kVersion) {
		std::cout << howdy::native::format_version(howdy::native::kProjectVersion,
		                                           howdy::native::kBuildCommit)
		          << "\n";
		return 0;
	}

	const bool needs_user_argument =
	    command_descriptor->user_target == howdy::native::UserTargetMode::kModelUser;
	if (needs_user_argument && !resolve_model_user(parsed, dependencies)) {
		return 1;
	}
	if (dependencies.effective_uid == nullptr ||
	    dependencies.effective_uid(dependencies.context) != 0) {
		std::cout << "This command requires root privileges.\n";
		std::cout << "Run it again with sudo.\n";
		return 1;
	}
	if (needs_user_argument) {
		if (!parsed.user.has_value()) {
			return 1;
		}
		if (parsed.user.value() == "root") {
			std::cout << "Running as root requires --user.\n";
			return 1;
		}
	}

	const auto selected_main =
	    dependencies.command_mains[static_cast<std::size_t>(command_descriptor->id)];
	if (selected_main == nullptr) {
		std::cout << "Unknown command: " << command_name << "\n";
		return 1;
	}

	auto argv_strings = build_command_argv_strings(*command_descriptor, parsed,
	                                               parsed.user.value_or(std::string{}));

	std::vector<char *> command_argv;
	command_argv.reserve(argv_strings.size() + 1);
	for (auto &value : argv_strings) {
		command_argv.push_back(value.data());
	}
	command_argv.push_back(nullptr);

	return selected_main(static_cast<int>(argv_strings.size()), command_argv.data());
}

auto howdy_main(int argc, char **argv) -> int {
	return howdy::native::howdy_internal::howdy_main_with_dependencies(
	    argc, argv,
	    {
	        .resolve_user  = resolve_user,
	        .effective_uid = effective_uid,
	        .command_mains = production_command_mains(),
	    });
}
