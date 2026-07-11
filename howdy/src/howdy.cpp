#include "cli/add_cli.hpp"
#include "cli/clear_cli.hpp"
#include "cli/config_cli.hpp"
#include "cli/disable_cli.hpp"
#include "cli/download_models_cli.hpp"
#include "cli/howdy_cli.hpp"
#include "cli/howdy_internal.hpp"
#include "cli/list_cli.hpp"
#include "cli/remove_cli.hpp"
#include "cli/set_cli.hpp"
#include "cli/snapshot_cli.hpp"
#include "cli/test_cli.hpp"
#include "common/user_names.hpp"

#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <pwd.h>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

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

	auto command_main(const howdy::native::howdy_internal::HowdyDependencies &dependencies,
	                  std::string_view command) -> howdy::native::howdy_internal::CommandMain {
		if (command == "add") {
			return dependencies.add;
		}
		if (command == "clear") {
			return dependencies.clear;
		}
		if (command == "config") {
			return dependencies.config;
		}
		if (command == "disable") {
			return dependencies.disable;
		}
		if (command == "download-models") {
			return dependencies.download_models;
		}
		if (command == "list") {
			return dependencies.list;
		}
		if (command == "remove") {
			return dependencies.remove;
		}
		if (command == "set") {
			return dependencies.set;
		}
		if (command == "snapshot") {
			return dependencies.snapshot;
		}
		if (command == "test") {
			return dependencies.test;
		}
		return nullptr;
	}

	void print_help() {
		std::cout << "usage: howdy [-U USER] [--plain] [-h] [-y] {command} [arguments...]\n\n";
		std::cout << "commands:\n";
		std::cout << "  add              Add face model\n";
		std::cout << "  clear            Remove all models\n";
		std::cout << "  config           Edit config\n";
		std::cout << "  disable          Enable or disable auth\n";
		std::cout << "  download-models  Download ONNX models\n";
		std::cout << "  list             List models\n";
		std::cout << "  remove           Remove a specific model\n";
		std::cout << "  set              Edit config value\n";
		std::cout << "  snapshot         Camera preview\n";
		std::cout << "  test             Test camera\n";
		std::cout << "  version          Print version\n";
		std::cout << "\noptions:\n";
		std::cout << "  -U, --user USER  Target user for model commands\n";
		std::cout << "  --plain          Disable interactive prompts where supported\n";
		std::cout << "  -y               Assume yes where supported\n";
		std::cout << "  -h, --help       Show this help\n";
	}

}  // namespace

int howdy::native::howdy_internal::howdy_main_with_dependencies(
    int argc, char **argv, const HowdyDependencies &dependencies) {
	std::string              user;
	bool                     yes   = false;
	bool                     plain = false;
	std::string              command;
	std::vector<std::string> arguments;

	for (int index = 1; index < argc; ++index) {
		const std::string_view arg(argv[index]);
		if (arg == "-U" || arg == "--user") {
			if (index + 1 < argc) {
				user = argv[++index];
			}
			continue;
		}
		if (arg == "-y") {
			yes = true;
			continue;
		}
		if (arg == "--plain") {
			plain = true;
			continue;
		}
		if (command.empty()) {
			if (arg == "-h" || arg == "--help") {
				print_help();
				return 0;
			}
			command = argv[index];
			continue;
		}
		arguments.emplace_back(argv[index]);
	}

	if (command.empty()) {
		print_help();
		return 0;
	}

	if (command == "version") {
		std::cout << "Howdy-Next 3.3.1\n";
		return 0;
	}

	if (user.empty()) {
		user = dependencies.resolve_user(dependencies.context);
	}
	if (user.empty()) {
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

	if (user == "root") {
		std::cout
		    << "Can't run howdy commands as root, please run this command with the --user flag\n";
		return 1;
	}

	const auto selected_main = command_main(dependencies, command);
	if (selected_main == nullptr) {
		std::cout << "Unknown command: " << command << "\n";
		return 1;
	}

	const bool needs_user_argument = command == "add" || command == "clear" || command == "list" ||
	                                 command == "remove" || command == "test";
	if (needs_user_argument && !howdy::native::is_valid_model_user_name(user)) {
		std::cout << howdy::native::kInvalidUserNameMessage << "\n";
		return 1;
	}

	std::vector<std::string> argv_strings;
	argv_strings.push_back("howdy-" + command);
	if (needs_user_argument) {
		argv_strings.push_back(user);
	}
	argv_strings.insert(argv_strings.end(), arguments.begin(), arguments.end());
	if (plain) {
		argv_strings.emplace_back("--plain");
	}
	if (yes) {
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

int howdy_main(int argc, char **argv) {
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
