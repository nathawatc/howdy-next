#include "cli/add_cli.hpp"
#include "cli/clear_cli.hpp"
#include "cli/howdy_internal.hpp"
#include "cli/list_cli.hpp"
#include "cli/remove_cli.hpp"

#include <array>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

	using howdy::native::howdy_internal::HowdyDependencies;

	enum class CommandId {
		kNone,
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
	};

	struct Context {
		std::string              resolved_user = "alice";
		uid_t                    effective_uid = 0;
		std::vector<std::string> command_arguments;
		int                      command_result = 0;
		CommandId                command_id     = CommandId::kNone;
	};

	Context *active_context = nullptr;

	auto resolve_user(void *raw_context) -> std::string {
		return static_cast<Context *>(raw_context)->resolved_user;
	}

	auto effective_uid(void *raw_context) -> uid_t {
		return static_cast<Context *>(raw_context)->effective_uid;
	}

	auto command_stub(CommandId command_id, int argc, char **argv) -> int {
		active_context->command_id = command_id;
		active_context->command_arguments.clear();
		for (int index = 0; index < argc; ++index) {
			active_context->command_arguments.emplace_back(argv[index]);
		}
		std::cout << "command output\n";
		std::cerr << "command error\n";
		return active_context->command_result;
	}

	auto add_stub(int argc, char **argv) -> int {
		return command_stub(CommandId::kAdd, argc, argv);
	}

	auto clear_stub(int argc, char **argv) -> int {
		return command_stub(CommandId::kClear, argc, argv);
	}

	auto config_stub(int argc, char **argv) -> int {
		return command_stub(CommandId::kConfig, argc, argv);
	}

	auto disable_stub(int argc, char **argv) -> int {
		return command_stub(CommandId::kDisable, argc, argv);
	}

	auto download_models_stub(int argc, char **argv) -> int {
		return command_stub(CommandId::kDownloadModels, argc, argv);
	}

	auto list_stub(int argc, char **argv) -> int {
		return command_stub(CommandId::kList, argc, argv);
	}

	auto remove_stub(int argc, char **argv) -> int {
		return command_stub(CommandId::kRemove, argc, argv);
	}

	auto set_stub(int argc, char **argv) -> int {
		return command_stub(CommandId::kSet, argc, argv);
	}

	auto snapshot_stub(int argc, char **argv) -> int {
		return command_stub(CommandId::kSnapshot, argc, argv);
	}

	auto test_stub(int argc, char **argv) -> int {
		return command_stub(CommandId::kTest, argc, argv);
	}

	struct RunResult {
		int         status;
		std::string output;
		std::string error;
	};

	auto run(Context &context, std::vector<std::string> arguments) -> RunResult {
		std::vector<char *> argv;
		argv.reserve(arguments.size());
		for (auto &argument : arguments) {
			argv.push_back(argument.data());
		}

		std::ostringstream output;
		std::ostringstream error;
		auto              *old_output = std::cout.rdbuf(output.rdbuf());
		auto              *old_error  = std::cerr.rdbuf(error.rdbuf());
		active_context                = &context;
		const auto status             = howdy::native::howdy_internal::howdy_main_with_dependencies(
		    static_cast<int>(argv.size()), argv.data(),
		    HowdyDependencies{
		        .context         = &context,
		        .resolve_user    = resolve_user,
		        .effective_uid   = effective_uid,
		        .add             = add_stub,
		        .clear           = clear_stub,
		        .config          = config_stub,
		        .disable         = disable_stub,
		        .download_models = download_models_stub,
		        .list            = list_stub,
		        .remove          = remove_stub,
		        .set             = set_stub,
		        .snapshot        = snapshot_stub,
		        .test            = test_stub,
		    });
		active_context = nullptr;
		std::cout.rdbuf(old_output);
		std::cerr.rdbuf(old_error);
		return {
		    .status = status,
		    .output = output.str(),
		    .error  = error.str(),
		};
	}

	auto expect(bool condition, std::string_view message) -> bool {
		if (!condition) {
			std::cerr << "FAIL: " << message << '\n';
		}
		return condition;
	}

}  // namespace

int main() {
	bool ok = true;

	{
		Context    context;
		const auto result = run(context, {"howdy"});
		ok &= expect(result.status == 0 && result.output.contains("usage: howdy"),
		             "no command prints help");
	}
	{
		Context    context;
		const auto result = run(context, {"howdy", "--help"});
		ok &= expect(result.status == 0 && result.output.contains("commands:"), "help succeeds");
	}
	{
		Context    context;
		const auto result = run(context, {"howdy", "version"});
		ok &= expect(result.status == 0 && result.output == "Howdy-Next 3.3.1\n",
		             "version output preserved");
	}
	{
		Context    context;
		const auto result = run(context, {"howdy", "unknown"});
		ok &= expect(result.status == 1 && result.output == "Unknown command: unknown\n",
		             "unknown command rejected");
	}
	{
		Context context;
		context.command_result = 23;
		const auto result =
		    run(context, {"howdy", "-U", "bob", "--plain", "-y", "add", "front-door"});
		ok &= expect(result.status == 23, "command return code passed through");
		ok &=
		    expect(context.command_arguments ==
		               std::vector<std::string>{"howdy-add", "bob", "front-door", "--plain", "-y"},
		           "global options forwarded with user injection");
		ok &= expect(result.output == "command output\n" && result.error == "command error\n",
		             "command output streams preserved");
	}
	{
		Context    context;
		const auto result = run(context, {"howdy", "-U", "bob", "config", "extra"});
		ok &= expect(result.status == 0, "non-user command dispatched");
		ok &= expect(context.command_arguments == std::vector<std::string>{"howdy-config", "extra"},
		             "user injected only for model commands");
	}
	{
		Context context;
		context.effective_uid = 1000;
		const auto result     = run(context, {"howdy", "list"});
		ok &=
		    expect(result.status == 1 && result.output.contains("Please run this command as root"),
		           "root check runs before dispatch");
		ok &= expect(context.command_arguments.empty(), "non-root command not dispatched");
	}
	{
		const std::vector<std::pair<std::string, CommandId>> commands = {
		    {"add", CommandId::kAdd},
		    {"clear", CommandId::kClear},
		    {"config", CommandId::kConfig},
		    {"disable", CommandId::kDisable},
		    {"download-models", CommandId::kDownloadModels},
		    {"list", CommandId::kList},
		    {"remove", CommandId::kRemove},
		    {"set", CommandId::kSet},
		    {"snapshot", CommandId::kSnapshot},
		    {"test", CommandId::kTest},
		};
		for (const auto &[command, expected_id] : commands) {
			Context    context;
			const auto result = run(context, {"howdy", "-U", "bob", command});
			ok &= expect(result.status == 0 && context.command_id == expected_id,
			             "command maps to matching entrypoint");
		}
	}
	{
		auto                  add_name = std::to_array("howdy-add");
		std::array<char *, 1> add_argv{add_name.data()};
		auto                  clear_name = std::to_array("howdy-clear");
		std::array<char *, 1> clear_argv{clear_name.data()};
		auto                  list_name = std::to_array("howdy-list");
		std::array<char *, 1> list_argv{list_name.data()};
		auto                  remove_name = std::to_array("howdy-remove");
		std::array<char *, 1> remove_argv{remove_name.data()};
		ok &= expect(add_main(1, add_argv.data()) == 1,
		             "add entrypoint returns on invalid arguments");
		ok &= expect(clear_main(1, clear_argv.data()) == 1,
		             "clear entrypoint returns on invalid arguments");
		ok &= expect(list_main(1, list_argv.data()) == 1,
		             "list entrypoint returns on invalid arguments");
		ok &= expect(remove_main(1, remove_argv.data()) == 1,
		             "remove entrypoint returns on invalid arguments");
	}

	return ok ? 0 : 1;
}
