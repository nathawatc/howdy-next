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
#include <cstdint>
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

	enum class CommandKind : std::uint8_t {
		kEntrypoint,
		kVersion,
	};

	using CommandDependency = CommandMain HowdyDependencies::*;

	struct CommandBinding {
		howdy::native::CommandId id;
		CommandKind              kind;
		CommandDependency        dependency;
	};

	constexpr std::array<CommandBinding, 11> kCommandBindings = {{
	    {
	        .id         = howdy::native::CommandId::kAdd,
	        .kind       = CommandKind::kEntrypoint,
	        .dependency = &HowdyDependencies::add,
	    },
	    {
	        .id         = howdy::native::CommandId::kClear,
	        .kind       = CommandKind::kEntrypoint,
	        .dependency = &HowdyDependencies::clear,
	    },
	    {
	        .id         = howdy::native::CommandId::kConfig,
	        .kind       = CommandKind::kEntrypoint,
	        .dependency = &HowdyDependencies::config,
	    },
	    {
	        .id         = howdy::native::CommandId::kDisable,
	        .kind       = CommandKind::kEntrypoint,
	        .dependency = &HowdyDependencies::disable,
	    },
	    {
	        .id         = howdy::native::CommandId::kDownloadModels,
	        .kind       = CommandKind::kEntrypoint,
	        .dependency = &HowdyDependencies::download_models,
	    },
	    {
	        .id         = howdy::native::CommandId::kList,
	        .kind       = CommandKind::kEntrypoint,
	        .dependency = &HowdyDependencies::list,
	    },
	    {
	        .id         = howdy::native::CommandId::kRemove,
	        .kind       = CommandKind::kEntrypoint,
	        .dependency = &HowdyDependencies::remove,
	    },
	    {
	        .id         = howdy::native::CommandId::kSet,
	        .kind       = CommandKind::kEntrypoint,
	        .dependency = &HowdyDependencies::set,
	    },
	    {
	        .id         = howdy::native::CommandId::kSnapshot,
	        .kind       = CommandKind::kEntrypoint,
	        .dependency = &HowdyDependencies::snapshot,
	    },
	    {
	        .id         = howdy::native::CommandId::kTest,
	        .kind       = CommandKind::kEntrypoint,
	        .dependency = &HowdyDependencies::test,
	    },
	    {
	        .id         = howdy::native::CommandId::kVersion,
	        .kind       = CommandKind::kVersion,
	        .dependency = nullptr,
	    },
	}};

	auto find_binding(howdy::native::CommandId id) -> const CommandBinding * {
		for (const auto &binding : kCommandBindings) {
			if (binding.id == id) {
				return &binding;
			}
		}
		return nullptr;
	}

	void print_completion_commands() {
		for (const auto &descriptor : howdy::native::command_catalog()) {
			std::cout << descriptor.name << '\n';
		}
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
			if (const auto *option = howdy::native::find_global_option(arg); option != nullptr) {
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
						if (parsed.command.empty()) {
							print_help();
							return 0;
						}
						break;
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
		if (parsed.global_option_seen || parsed.arguments.size() != 1 ||
		    parsed.arguments.front() != "commands") {
			return 1;
		}
		print_completion_commands();
		return 0;
	}

	const auto *command_descriptor = howdy::native::find_command(parsed.command);
	const auto *command_binding =
	    command_descriptor == nullptr ? nullptr : find_binding(command_descriptor->id);
	if (command_binding != nullptr && command_binding->kind == CommandKind::kVersion) {
		std::cout << "Howdy-Next " << howdy::native::kProjectVersion << "\n";
		return 0;
	}

	if (parsed.user.empty()) {
		parsed.user = dependencies.resolve_user(dependencies.context);
	}
	if (parsed.user.empty()) {
		std::cout << "Could not determine user, please use the --user flag\n";
		return 1;
	}

	if (dependencies.effective_uid(dependencies.context) != 0) {
		std::cout << "Please run this command as root:\n\n";
		std::cout << "\tsudo howdy";
		for (int index = 1; index < argc; ++index) {
			std::cout << " " << argv[index];
		}
		std::cout << "\n";
		return 1;
	}

	if (parsed.user == "root") {
		std::cout
		    << "Can't run howdy commands as root, please run this command with the --user flag\n";
		return 1;
	}

	auto selected_main = static_cast<CommandMain>(nullptr);
	if (command_binding != nullptr && command_binding->kind == CommandKind::kEntrypoint) {
		selected_main = dependencies.*(command_binding->dependency);
	}
	if (selected_main == nullptr) {
		std::cout << "Unknown command: " << parsed.command << "\n";
		return 1;
	}

	const bool needs_user_argument = command_descriptor->accepts_user_argument;
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
	        .resolve_user    = resolve_user,
	        .effective_uid   = effective_uid,
	        .add             = add_main,
	        .clear           = clear_main,
	        .config          = config_main,
	        .disable         = disable_main,
	        .download_models = download_models_main,
	        .list            = list_main,
	        .remove          = remove_main,
	        .set             = set_main,
	        .snapshot        = snapshot_main,
	        .test            = test_main,
	    });
}
