#include "app/command_catalog.hpp"
#include "app/howdy_internal.hpp"
#include "cli/add_cli.hpp"
#include "cli/clear_cli.hpp"
#include "cli/list_cli.hpp"
#include "cli/remove_cli.hpp"
#include "test_support.hpp"
#include "version.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

	using howdy::test::expect;

	using howdy::native::command_catalog;
	using howdy::native::CommandId;
	using howdy::native::howdy_internal::CommandMain;
	using howdy::native::howdy_internal::HowdyDependencies;

	struct Context {
		std::string              resolved_user = "alice";
		uid_t                    effective_uid = 0;
		std::vector<std::string> command_arguments;
		int                      command_result = 0;
		std::optional<CommandId> command_id;
		int                      resolve_user_calls  = 0;
		int                      effective_uid_calls = 0;
	};

	Context *active_context = nullptr;

	auto resolve_user(void *raw_context) -> std::string {
		auto &context = *static_cast<Context *>(raw_context);
		++context.resolve_user_calls;
		return context.resolved_user;
	}

	auto effective_uid(void *raw_context) -> uid_t {
		auto &context = *static_cast<Context *>(raw_context);
		++context.effective_uid_calls;
		return context.effective_uid;
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

	auto run(Context &context, std::vector<std::string> arguments,
	         CommandMain list_callback = list_stub) -> RunResult {
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
		        .list            = list_callback,
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

	auto test_completion_behavior() -> bool {
		bool ok = true;

		{
			Context    context;
			const auto result = run(context, {"howdy", "--help"});
			ok &=
			    expect(result.status == 0 && result.output.contains("commands:"), "help succeeds");
			ok &=
			    expect(result.output.starts_with(
			               "usage: howdy [-U USER] [--plain] [-h] [-y] {command} [arguments...]\n"),
			           "help usage ordering is preserved");
			for (const auto &command : command_catalog()) {
				ok &= expect(result.output.contains(command.name),
				             "help lists every catalog command");
				ok &= expect(result.output.contains(command.summary),
				             "help uses every catalog summary");
			}
			for (const auto &option : howdy::native::global_option_catalog()) {
				ok &= expect(result.output.contains(option.summary),
				             "help uses every catalog option summary");
			}
			ok &=
			    expect(!result.output.contains("__complete"), "completion query stays out of help");
		}
		{
			Context     context;
			const auto  result = run(context, {"howdy", "__complete", "commands"});
			std::string expected;
			for (const auto &command : command_catalog()) {
				expected += command.name;
				expected += '\n';
			}
			ok &= expect(result.status == 0 && result.output == expected && result.error.empty(),
			             "completion query returns canonical catalog command list");
			ok &= expect(context.resolve_user_calls == 0 && context.effective_uid_calls == 0,
			             "completion query skips user and root checks");
			ok &= expect(!context.command_id.has_value() && context.command_arguments.empty(),
			             "completion query does not dispatch a command");
		}
		{
			const std::vector<std::vector<std::string>> malformed_queries = {
			    {"howdy", "__complete"},
			    {"howdy", "__complete", "unknown"},
			    {"howdy", "__complete", "commands", "extra"},
			    {"howdy", "__complete", "commands", "-y"},
			    {"howdy", "-y", "__complete", "commands"},
			    {"howdy", "__complete", "--plain", "commands"},
			    {"howdy", "__complete", "commands", "--plain"},
			    {"howdy", "__complete", "commands", "-U", "bob"},
			    {"howdy", "-U", "bob", "__complete", "commands"},
			    {"howdy", "--user", "bob", "__complete", "commands"},
			};
			for (const auto &arguments : malformed_queries) {
				Context    context;
				const auto result = run(context, arguments);
				ok &= expect(result.status != 0, "malformed completion query is rejected");
				ok &= expect(!result.output.contains("add\n"),
				             "malformed completion query prints no command list");
				ok &= expect(context.resolve_user_calls == 0 && context.effective_uid_calls == 0,
				             "malformed completion query skips user and root checks");
				ok &= expect(!context.command_id.has_value() && context.command_arguments.empty(),
				             "malformed completion query does not dispatch a command");
			}
		}
		{
			Context    context;
			const auto result = run(context, {"howdy", "version"});
			const auto expected =
			    "Howdy-Next " + std::string(howdy::native::kProjectVersion) + "\n";
			ok &=
			    expect(result.status == 0 && result.output == expected, "version output preserved");
			ok &= expect(context.resolve_user_calls == 0 && context.effective_uid_calls == 0,
			             "version skips user and root checks");
			ok &= expect(!context.command_id.has_value() && context.command_arguments.empty(),
			             "version does not dispatch a command");
		}

		return ok;
	}

	auto test_missing_callback() -> bool {
		Context    context;
		const auto result = run(context, {"howdy", "-U", "alice", "list"}, nullptr);
		bool       ok     = true;
		ok &= expect(result.status != 0, "null command callback returns failure");
		ok &= expect(result.output == "Unknown command: list\n",
		             "null command callback keeps diagnostic");
		ok &= expect(context.resolve_user_calls == 0,
		             "explicit user skips lookup for null command callback");
		ok &= expect(context.effective_uid_calls == 1,
		             "null callback is checked after root validation");
		ok &= expect(!context.command_id.has_value() && context.command_arguments.empty(),
		             "null command callback is not invoked");
		return ok;
	}

}  // namespace

auto main() -> int {
	bool ok = true;

	{
		Context    context;
		const auto result = run(context, {"howdy"});
		ok &= expect(result.status == 0 && result.output.contains("usage: howdy"),
		             "no command prints help");
	}
	ok &= test_completion_behavior();
	ok &= test_missing_callback();
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
		ok &= expect(context.resolve_user_calls == 0,
		             "short user option skips default user resolution");
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
		context.command_result = 23;
		const auto result      = run(context, {"howdy", "list"});
		ok &= expect(result.status == 23, "default-user command return code passed through");
		ok &= expect(context.resolve_user_calls == 1,
		             "default user resolved when user option is omitted");
		ok &= expect(context.command_id == CommandId::kList, "list dispatched with default user");
		ok &= expect(context.command_arguments == std::vector<std::string>{"howdy-list", "alice"},
		             "resolved default user injected into list arguments");
	}
	{
		Context    context;
		const auto result = run(context, {"howdy", "--user", "bob", "list"});
		ok &= expect(result.status == 0, "long user option command dispatched");
		ok &= expect(context.resolve_user_calls == 0,
		             "long user option skips default user resolution");
		ok &= expect(context.command_id == CommandId::kList, "long user option dispatches list");
		ok &= expect(context.command_arguments == std::vector<std::string>{"howdy-list", "bob"},
		             "long user option injected into list arguments");
	}
	{
		Context    context;
		const auto result = run(context, {"howdy", "list", ""});
		ok &= expect(result.status == 0, "empty positional argument does not change dispatch");
		ok &=
		    expect(context.command_arguments == std::vector<std::string>{"howdy-list", "alice", ""},
		           "empty positional argument remains forwarded");
		ok &= expect(std::ranges::find(context.command_arguments, "--plain") ==
		                 context.command_arguments.end(),
		             "empty positional argument does not enable plain mode");
	}
	{
		Context    context;
		const auto result = run(context, {"howdy", "-U"});
		ok &= expect(result.status == 1, "trailing short user option rejected");
		ok &= expect(result.output.contains("-U") && result.output.contains("requires an argument"),
		             "trailing short user option reports missing argument");
		ok &= expect(!result.output.contains("usage: howdy"),
		             "trailing short user option does not print help");
		ok &= expect(context.resolve_user_calls == 0 && context.effective_uid_calls == 0,
		             "trailing short user option skips user resolution and root check");
		ok &= expect(!context.command_id.has_value() && context.command_arguments.empty(),
		             "trailing short user option does not dispatch command");
	}
	{
		Context    context;
		const auto result = run(context, {"howdy", "--user"});
		ok &= expect(result.status == 1, "trailing long user option rejected");
		ok &= expect(result.output.contains("--user") &&
		                 result.output.contains("requires an argument"),
		             "trailing long user option reports missing argument");
		ok &= expect(!result.output.contains("usage: howdy"),
		             "trailing long user option does not print help");
		ok &= expect(context.resolve_user_calls == 0 && context.effective_uid_calls == 0,
		             "trailing long user option skips user resolution and root check");
		ok &= expect(!context.command_id.has_value() && context.command_arguments.empty(),
		             "trailing long user option does not dispatch command");
	}
	{
		Context    context;
		const auto result = run(context, {"howdy", "list", "-U"});
		ok &= expect(result.status == 1, "trailing short user option after command rejected");
		ok &= expect(result.output.contains("-U") && result.output.contains("requires an argument"),
		             "trailing short user option after command reports missing argument");
		ok &=
		    expect(context.resolve_user_calls == 0 && context.effective_uid_calls == 0,
		           "trailing short user option after command skips user resolution and root check");
		ok &= expect(!context.command_id.has_value() && context.command_arguments.empty(),
		             "trailing short user option after command does not dispatch command");
	}
	{
		Context    context;
		const auto result = run(context, {"howdy", "add", "--user"});
		ok &= expect(result.status == 1, "trailing long user option after command rejected");
		ok &= expect(result.output.contains("--user") &&
		                 result.output.contains("requires an argument"),
		             "trailing long user option after command reports missing argument");
		ok &=
		    expect(context.resolve_user_calls == 0 && context.effective_uid_calls == 0,
		           "trailing long user option after command skips user resolution and root check");
		ok &= expect(!context.command_id.has_value() && context.command_arguments.empty(),
		             "trailing long user option after command does not dispatch command");
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
		std::size_t dispatched_commands = 0;
		for (const auto &command : command_catalog()) {
			if (command.id == CommandId::kVersion) {
				continue;
			}
			++dispatched_commands;
			Context    context;
			const auto result = run(context, {"howdy", "-U", "bob", std::string(command.name)});
			ok &= expect(result.status == 0 && context.command_id == command.id,
			             "every catalog command maps to matching entrypoint");
		}
		ok &= expect(dispatched_commands + 1 == command_catalog().size(),
		             "every production command is covered by dispatch test");
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
