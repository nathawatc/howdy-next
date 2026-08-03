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

	struct CommandSpec {
		std::string_view  name;
		std::string_view  description;
		CommandKind       kind;
		CommandDependency dependency;
		bool              needs_user_argument;
	};

	constexpr std::array<CommandSpec, 11> kCommandSpecs = {{
	    {
	        .name                = "add",
	        .description         = "Add face model",
	        .kind                = CommandKind::kEntrypoint,
	        .dependency          = &HowdyDependencies::add,
	        .needs_user_argument = true,
	    },
	    {
	        .name                = "clear",
	        .description         = "Remove all models",
	        .kind                = CommandKind::kEntrypoint,
	        .dependency          = &HowdyDependencies::clear,
	        .needs_user_argument = true,
	    },
	    {
	        .name                = "config",
	        .description         = "Edit config",
	        .kind                = CommandKind::kEntrypoint,
	        .dependency          = &HowdyDependencies::config,
	        .needs_user_argument = false,
	    },
	    {
	        .name                = "disable",
	        .description         = "Enable or disable auth",
	        .kind                = CommandKind::kEntrypoint,
	        .dependency          = &HowdyDependencies::disable,
	        .needs_user_argument = false,
	    },
	    {
	        .name                = "download-models",
	        .description         = "Download ONNX models",
	        .kind                = CommandKind::kEntrypoint,
	        .dependency          = &HowdyDependencies::download_models,
	        .needs_user_argument = false,
	    },
	    {
	        .name                = "list",
	        .description         = "List models",
	        .kind                = CommandKind::kEntrypoint,
	        .dependency          = &HowdyDependencies::list,
	        .needs_user_argument = true,
	    },
	    {
	        .name                = "remove",
	        .description         = "Remove a specific model",
	        .kind                = CommandKind::kEntrypoint,
	        .dependency          = &HowdyDependencies::remove,
	        .needs_user_argument = true,
	    },
	    {
	        .name                = "set",
	        .description         = "Edit config value",
	        .kind                = CommandKind::kEntrypoint,
	        .dependency          = &HowdyDependencies::set,
	        .needs_user_argument = false,
	    },
	    {
	        .name                = "snapshot",
	        .description         = "Camera preview",
	        .kind                = CommandKind::kEntrypoint,
	        .dependency          = &HowdyDependencies::snapshot,
	        .needs_user_argument = false,
	    },
	    {
	        .name                = "test",
	        .description         = "Test camera",
	        .kind                = CommandKind::kEntrypoint,
	        .dependency          = &HowdyDependencies::test,
	        .needs_user_argument = true,
	    },
	    {
	        .name                = "version",
	        .description         = "Print version",
	        .kind                = CommandKind::kVersion,
	        .dependency          = nullptr,
	        .needs_user_argument = false,
	    },
	}};

	auto find_command(std::string_view name) -> const CommandSpec * {
		for (const auto &spec : kCommandSpecs) {
			if (spec.name == name) {
				return &spec;
			}
		}
		return nullptr;
	}

	void print_completion_commands() {
		for (const auto &spec : kCommandSpecs) {
			std::cout << spec.name << '\n';
		}
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

	void print_help() {
		std::cout << "usage: howdy [-U USER] [--plain] [-h] [-y] {command} [arguments...]\n\n";
		std::cout << "commands:\n";
		for (const auto &spec : kCommandSpecs) {
			std::cout << "  " << std::left << std::setw(17) << spec.name << spec.description
			          << '\n';
		}
		std::cout << "\noptions:\n";
		std::cout << "  -U, --user USER  Target user for model commands\n";
		std::cout << "  --plain          Disable interactive prompts where supported\n";
		std::cout << "  -y               Assume yes where supported\n";
		std::cout << "  -h, --help       Show this help\n";
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
			if (arg == "-U" || arg == "--user") {
				parsed.global_option_seen = true;
				if (index + 1 >= argc) {
					std::cout << "Option '" << arg << "' requires an argument\n";
					return 1;
				}
				parsed.user = argv[++index];
				continue;
			}
			if (arg == "-y") {
				parsed.global_option_seen = true;
				parsed.yes                = true;
				continue;
			}
			if (arg == "--plain") {
				parsed.global_option_seen = true;
				parsed.plain              = true;
				continue;
			}
			if (parsed.command.empty()) {
				if (arg == "-h" || arg == "--help") {
					print_help();
					return 0;
				}
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

	const auto *command_spec = find_command(parsed.command);
	if (command_spec != nullptr && command_spec->kind == CommandKind::kVersion) {
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
	if (command_spec != nullptr && command_spec->kind == CommandKind::kEntrypoint) {
		selected_main = dependencies.*(command_spec->dependency);
	}
	if (selected_main == nullptr) {
		std::cout << "Unknown command: " << parsed.command << "\n";
		return 1;
	}

	const bool needs_user_argument = command_spec->needs_user_argument;
	if (needs_user_argument && !howdy::native::is_valid_model_user_name(parsed.user)) {
		std::cout << howdy::native::kInvalidUserNameMessage << "\n";
		return 1;
	}

	std::vector<std::string> argv_strings;
	argv_strings.push_back("howdy-" + std::string(command_spec->name));
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
