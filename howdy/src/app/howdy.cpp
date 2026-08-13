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
#include "support/user_names.hpp"
#include "version.hpp"

#include <array>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <pwd.h>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

	using howdy::native::howdy_internal::CommandMain;
	using howdy::native::howdy_internal::HowdyDependencies;

	constexpr std::array<std::string_view, 2> kBooleanCompletionValues{"false", "true"};

	void print_completion_commands() {
		for (const auto &descriptor : howdy::native::command_catalog()) {
			std::cout << descriptor.name << '\n';
		}
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

	auto parse_completion_position(std::string_view text, std::size_t &position) -> bool {
		if (text.empty()) {
			return false;
		}
		const auto result = std::from_chars(text.data(), text.data() + text.size(), position);
		return result.ec == std::errc{} && result.ptr == text.data() + text.size();
	}

	auto print_completion_command_values(std::string_view command_name, std::size_t position)
	    -> bool {
		const auto *descriptor = howdy::native::find_command(command_name);
		if (descriptor == nullptr || position != 0) {
			return false;
		}
		if (descriptor->completion == howdy::native::CommandCompletionKind::kBoolean) {
			for (const auto value : kBooleanCompletionValues) {
				std::cout << value << '\n';
			}
		}
		return true;
	}

	auto handle_completion_query(const std::vector<std::string> &arguments, bool global_option_seen)
	    -> int {
		if (global_option_seen) {
			return 1;
		}
		if (arguments.size() == 1 && arguments.front() == "commands") {
			print_completion_commands();
			return 0;
		}
		if (arguments.size() == 1 && arguments.front() == "global-options") {
			print_completion_global_options();
			return 0;
		}
		if (arguments.size() == 3 && arguments.front() == "command-values") {
			std::size_t position = 0;
			if (parse_completion_position(arguments[2], position) &&
			    print_completion_command_values(arguments[1], position)) {
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

	constexpr std::array kUsageOptionOrder{
	    howdy::native::GlobalOptionId::kUser,
	    howdy::native::GlobalOptionId::kPlain,
	    howdy::native::GlobalOptionId::kHelp,
	    howdy::native::GlobalOptionId::kYes,
	};

	void print_help() {
		std::cout << "usage: howdy";
		for (const auto id : kUsageOptionOrder) {
			if (const auto *option = howdy::native::find_global_option(id); option != nullptr) {
				std::cout << " " << format_usage_option(*option);
			}
		}
		std::cout << " {command} [arguments...]\n\n";
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

	struct ParsedCommandLine {
		std::string              user;
		bool                     yes                = false;
		bool                     plain              = false;
		bool                     global_option_seen = false;
		std::string              command;
		std::vector<std::string> arguments;
	};

	auto parse_command_line(int argc, char **argv, ParsedCommandLine &parsed)
	    -> std::optional<int> {
		for (int index = 1; index < argc; ++index) {
			const std::string_view arg(argv[index]);
			if (const auto *option = howdy::native::find_global_option(arg);
			    option != nullptr && (parsed.command.empty() || option->parses_after_command)) {
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
						print_help();
						return 0;
					case howdy::native::GlobalOptionId::kCount:
						break;
				}
			}
			if (parsed.command.empty()) {
				parsed.command = argv[index];
				continue;
			}
			parsed.arguments.emplace_back(argv[index]);
		}
		return std::nullopt;
	}

}  // namespace

auto howdy::native::howdy_internal::howdy_main_with_dependencies(
    int argc, char **argv, const HowdyDependencies &dependencies) -> int {
	ParsedCommandLine parsed;
	if (const auto parse_result = parse_command_line(argc, argv, parsed);
	    parse_result.has_value()) {
		return *parse_result;
	}

	if (parsed.command.empty()) {
		print_help();
		return 0;
	}

	if (parsed.command == "__complete") {
		return handle_completion_query(parsed.arguments, parsed.global_option_seen);
	}

	const auto *command_descriptor = howdy::native::find_command(parsed.command);
	if (command_descriptor != nullptr &&
	    command_descriptor->kind == howdy::native::CommandKind::kVersion) {
		std::cout << "Howdy-Next " << howdy::native::kProjectVersion << "\n";
		return 0;
	}

	if (parsed.user.empty()) {
		parsed.user = dependencies.resolve_user(dependencies.context);
	}
	if (parsed.user.empty()) {
		std::cout << "Unable to determine the user; please use --user\n";
		return 1;
	}

	if (dependencies.effective_uid(dependencies.context) != 0) {
		std::cout << "This command requires root privileges.\n\n";
		std::cout << "\tsudo howdy";
		for (int index = 1; index < argc; ++index) {
			std::cout << " " << argv[index];
		}
		std::cout << "\n";
		return 1;
	}

	if (parsed.user == "root") {
		std::cout << "Running as root requires --user.\n";
		return 1;
	}

	auto selected_main = static_cast<CommandMain>(nullptr);
	if (command_descriptor != nullptr &&
	    command_descriptor->kind == howdy::native::CommandKind::kEntrypoint) {
		selected_main =
		    dependencies.command_mains[static_cast<std::size_t>(command_descriptor->id)];
	}
	if (selected_main == nullptr) {
		std::cout << "Unknown command: " << parsed.command << "\n";
		return 1;
	}

	const bool needs_user_argument =
	    command_descriptor->user_target == howdy::native::UserTargetMode::kModelUser;
	if (needs_user_argument && !howdy::native::is_valid_model_user_name(parsed.user)) {
		std::cout << howdy::native::kInvalidUserNameMessage << "\n";
		return 1;
	}

	std::vector<std::string> argv_strings;
	argv_strings.push_back("howdy-" + std::string(command_descriptor->name));
	if (needs_user_argument) {
		argv_strings.push_back(parsed.user);
	}
	argv_strings.insert(argv_strings.end(), parsed.arguments.begin(), parsed.arguments.end());
	if (parsed.plain) {
		argv_strings.emplace_back("--plain");
	}
	if (parsed.yes) {
		argv_strings.emplace_back("-y");
	}

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
