#include "app/command_catalog.hpp"
#include "app/howdy_cli.hpp"
#include "app/howdy_cli_internal.hpp"
#include "app/howdy_completion_internal.hpp"
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
#include "howdy/version_format.hpp"
#include "support/invoking_user.hpp"
#include "support/user_names.hpp"
#include "version.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <pwd.h>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

	using howdy::native::howdy_cli_internal::CliSyntaxError;
	using howdy::native::howdy_cli_internal::CliSyntaxErrorKind;
	using howdy::native::howdy_cli_internal::ParsedCommandLine;
	using howdy::native::howdy_internal::CommandMain;
	using howdy::native::howdy_internal::HowdyDependencies;

	auto resolve_user(void *context) -> std::string {
		(void)context;
		for (const char *name : {"SUDO_USER", "DOAS_USER"}) {
			if (const char *value = std::getenv(name); value != nullptr && value[0] != '\0') {
				return value;
			}
		}

		if (const auto pkexec_uid = howdy::native::parse_uid_env(std::getenv("PKEXEC_UID"))) {
			if (passwd *pwd = getpwuid(*pkexec_uid); pwd != nullptr) {
				return {pwd->pw_name};
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

	auto handle_special_command(const ParsedCommandLine &parsed) -> std::optional<int> {
		if (!parsed.command.has_value()) {
			howdy::native::howdy_cli_internal::print_help();
			return 0;
		}
		if (parsed.help_requested && !parsed.help_after_command) {
			howdy::native::howdy_cli_internal::print_help();
			return 0;
		}
		if (*parsed.command != "__complete") {
			return std::nullopt;
		}
		std::vector<std::string> completion_arguments;
		completion_arguments.reserve(parsed.arguments.size());
		for (const auto &argument : parsed.arguments) {
			completion_arguments.push_back(argument.value);
		}
		return howdy::native::howdy_completion_internal::handle_completion_query(
		    completion_arguments, parsed.global_option_seen);
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
	if (const auto parse_error =
	        howdy::native::howdy_cli_internal::parse_command_line(argc, argv, parsed);
	    parse_error.has_value()) {
		const auto *command =
		    parsed.command.has_value() ? howdy::native::find_command(*parsed.command) : nullptr;
		return howdy::native::howdy_cli_internal::print_cli_syntax_error(*parse_error, command);
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
		return howdy::native::howdy_cli_internal::print_cli_syntax_error(
		    CliSyntaxError{
		        .kind  = CliSyntaxErrorKind::kUnrecognizedSubcommand,
		        .value = command_name,
		    },
		    nullptr);
	}
	if (parsed.help_requested && parsed.help_after_command) {
		howdy::native::howdy_cli_internal::print_command_help(*command_descriptor);
		return 0;
	}
	if (const auto error =
	        howdy::native::howdy_cli_internal::command_syntax_error(parsed, *command_descriptor);
	    error.has_value()) {
		return howdy::native::howdy_cli_internal::print_cli_syntax_error(*error,
		                                                                 command_descriptor);
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
		std::cerr << "howdy: command entrypoint unavailable: " << command_name << '\n';
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
