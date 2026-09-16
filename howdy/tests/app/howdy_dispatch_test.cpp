#include "app/command_catalog.hpp"
#include "app/howdy/internal.hpp"
#include "app/howdy_dispatch_test_support.hpp"
#include "test_support.hpp"

#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace howdy::test::dispatch {

	namespace {

		using howdy::native::CommandId;
		using howdy::native::howdy_internal::CommandMain;
		using howdy::native::howdy_internal::HowdyDependencies;

		Context *active_context = nullptr;

		auto ResolveInvokingIdentity(void *raw_context) -> howdy::native::InvokingIdentityResult {
			auto &context = *static_cast<Context *>(raw_context);
			++context.resolve_user_calls;
			if (context.identity_status != howdy::native::InvokingIdentityStatus::kResolved) {
				return {.status = context.identity_status};
			}
			return {
			    .status = howdy::native::InvokingIdentityStatus::kResolved,
			    .user   = howdy::native::InvokingUser{.name = context.resolved_user},
			};
		}

		auto EffectiveUid(void *raw_context) -> uid_t {
			auto &context = *static_cast<Context *>(raw_context);
			++context.effective_uid_calls;
			return context.effective_uid;
		}

		auto CommandStub(CommandId command_id, int argc, char **argv) -> int {
			active_context->command_id = command_id;
			active_context->command_arguments.clear();
			for (int index = 0; index < argc; ++index) {
				active_context->command_arguments.emplace_back(argv[index]);
			}
			std::cout << "command output\n";
			std::cerr << "command error\n";
			return active_context->command_result;
		}

		auto AddStub(int argc, char **argv) -> int {
			return CommandStub(CommandId::kAdd, argc, argv);
		}

		auto ClearStub(int argc, char **argv) -> int {
			return CommandStub(CommandId::kClear, argc, argv);
		}

		auto ConfigStub(int argc, char **argv) -> int {
			return CommandStub(CommandId::kConfig, argc, argv);
		}

		auto DisableStub(int argc, char **argv) -> int {
			return CommandStub(CommandId::kDisable, argc, argv);
		}

		auto DownloadModelsStub(int argc, char **argv) -> int {
			return CommandStub(CommandId::kDownloadModels, argc, argv);
		}

		auto ListStub(int argc, char **argv) -> int {
			return CommandStub(CommandId::kList, argc, argv);
		}

		auto RemoveStub(int argc, char **argv) -> int {
			return CommandStub(CommandId::kRemove, argc, argv);
		}

		auto SetStub(int argc, char **argv) -> int {
			return CommandStub(CommandId::kSet, argc, argv);
		}

		auto SnapshotStub(int argc, char **argv) -> int {
			return CommandStub(CommandId::kSnapshot, argc, argv);
		}

		auto TestStub(int argc, char **argv) -> int {
			return CommandStub(CommandId::kTest, argc, argv);
		}

		auto DefaultCommandMains() -> CommandMains {
			return {
			    AddStub,  ClearStub,  ConfigStub, DisableStub,  DownloadModelsStub,
			    ListStub, RemoveStub, SetStub,    SnapshotStub, TestStub,
			    nullptr,
			};
		}

	}  // namespace

	auto Run(Context &context, std::vector<std::string> arguments) -> RunResult {
		return Run(context, std::move(arguments), ListStub, DefaultCommandMains());
	}

	auto Run(Context &context, std::vector<std::string> arguments, CommandMain list_callback)
	    -> RunResult {
		return Run(context, std::move(arguments), list_callback, DefaultCommandMains());
	}

	auto Run(Context &context, std::vector<std::string> arguments, CommandMain list_callback,
	         CommandMains command_mains) -> RunResult {
		std::vector<char *> argv;
		argv.reserve(arguments.size());
		for (auto &argument : arguments) {
			argv.push_back(argument.data());
		}

		std::ostringstream output;
		std::ostringstream error;
		auto              *old_output                             = std::cout.rdbuf(output.rdbuf());
		auto              *old_error                              = std::cerr.rdbuf(error.rdbuf());
		active_context                                            = &context;
		command_mains[static_cast<std::size_t>(CommandId::kList)] = list_callback;
		const auto status = howdy::native::howdy_internal::HowdyMainWithDependencies(
		    static_cast<int>(argv.size()), argv.data(),
		    HowdyDependencies{
		        .context                   = &context,
		        .resolve_invoking_identity = ResolveInvokingIdentity,
		        .effective_uid             = EffectiveUid,
		        .command_mains             = command_mains,
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

}  // namespace howdy::test::dispatch

namespace {

	using howdy::test::Expect;

	using howdy::native::CommandCatalog;
	using howdy::native::CommandId;
	using howdy::native::CommandKind;
	using howdy::test::dispatch::Context;
	using howdy::test::dispatch::Run;

	auto TestMissingCallback() -> bool {
		Context    context;
		const auto result = Run(context, {"howdy", "-U", "alice", "list"}, nullptr);
		bool       ok     = true;
		ok &= Expect(result.status == 1, "null command callback returns runtime failure");
		ok &= Expect(result.output.empty() &&
		                 result.error == "howdy: command entrypoint unavailable: list\n",
		             "null command callback reports internal entrypoint failure on stderr");
		ok &= Expect(context.resolve_user_calls == 0,
		             "explicit user skips lookup for null command callback");
		ok &= Expect(context.effective_uid_calls == 1,
		             "null callback is checked after root validation");
		ok &= Expect(!context.command_id.has_value() && context.command_arguments.empty(),
		             "null command callback is not invoked");
		return ok;
	}

	auto TestStrictSyntaxCases() -> bool {
		bool ok = true;
		for (const auto &arguments : std::vector<std::vector<std::string>>{
		         {"howdy", "add", "one", "two"},
		         {"howdy", "clear", "typo", "-y"},
		         {"howdy", "config", "typo"},
		         {"howdy", "disable", "true", "typo"},
		         {"howdy", "download-models", "typo"},
		         {"howdy", "list", "typo"},
		         {"howdy", "remove", "3", "typo", "-y"},
		         {"howdy", "set", "device_path", "/dev/video0", "typo"},
		         {"howdy", "snapshot", "typo"},
		         {"howdy", "test", "--device"},
		         {"howdy", "test", "--device", ""},
		         {"howdy", "test", "--device", "--plain"},
		         {"howdy", "test", "--unknown"},
		         {"howdy", "version", "typo"},
		     }) {
			Context    context;
			const auto result = Run(context, arguments);
			ok &= Expect(result.status == 2 && result.output.empty() &&
			                 result.error.starts_with("error: ") && !context.command_id.has_value(),
			             "strict command syntax uses clap-style usage failure before callback");
			ok &= Expect(context.resolve_user_calls == 0 && context.effective_uid_calls == 0,
			             "malformed syntax skips user and privilege work");
		}
		{
			Context    context;
			const auto result = Run(
			    context, {"howdy", "test", "--device", "/dev/video0", "--device", "/dev/video1"});
			ok &= Expect(result.status == 2 &&
			                 result.error.contains("cannot be used multiple times") &&
			                 !context.command_id.has_value(),
			             "duplicate test device option is rejected as usage error");
		}
		return ok;
	}

	auto TestUnknownCommandPrecedence() -> bool {
		bool ok = true;
		for (const uid_t effective_uid : {static_cast<uid_t>(1000), static_cast<uid_t>(0)}) {
			Context context;
			context.effective_uid = effective_uid;
			context.resolved_user = "root";
			const auto result     = Run(context, {"howdy", "vers"});
			ok &= Expect(result.status == 2 && result.output.empty() &&
			                 result.error.starts_with("error: unrecognized subcommand 'vers'\n"),
			             "unknown command uses clap-style subcommand error before other checks");
			ok &= Expect(context.resolve_user_calls == 0 && context.effective_uid_calls == 0 &&
			                 !context.command_id.has_value(),
			             "unknown command performs no user or privilege work");
		}
		return ok;
	}

	auto TestEmptyUserOptions() -> bool {
		bool ok = true;
		for (const auto &user_option : {std::string{"-U"}, std::string{"--user"}}) {
			Context    context;
			const auto result = Run(context, {"howdy", user_option, "", "clear", "-y"});
			ok &= Expect(result.status == 2 && result.output.empty() &&
			                 result.error.contains("value cannot be empty"),
			             "explicit empty user is rejected as usage error");
			ok &= Expect(context.resolve_user_calls == 0 && context.effective_uid_calls == 0 &&
			                 !context.command_id.has_value(),
			             "explicit empty user cannot fall back or execute");
		}
		return ok;
	}

	auto TestClapStyleUsageErrors() -> bool {
		bool ok = true;
		{
			Context    context;
			const auto result = Run(context, {"howdy", "-t"});
			ok &= Expect(result.status == 2 && result.output.empty() &&
			                 result.error == "error: unexpected argument '-t' found\n\n"
			                                 "Usage: howdy [OPTIONS] <COMMAND>\n\n"
			                                 "For more information, try '--help'.\n",
			             "unknown top-level option uses clap-style error contract");
		}
		{
			Context    context;
			const auto result = Run(context, {"howdy", "clear", "-t"});
			ok &= Expect(result.status == 2 && result.output.empty() &&
			                 result.error.contains("error: unexpected argument '-t' found\n") &&
			                 result.error.contains("Usage: howdy clear [OPTIONS]\n") &&
			                 !result.error.contains("tip:"),
			             "option error uses command usage without inapplicable double-dash tip");
		}
		{
			Context    context;
			const auto result = Run(context, {"howdy", "clear", "typo"});
			ok &= Expect(result.status == 2 && result.output.empty() &&
			                 result.error.contains("error: unexpected argument 'typo' found\n") &&
			                 result.error.contains("Usage: howdy clear [OPTIONS]\n"),
			             "surplus positional reports concrete unexpected argument");
		}
		{
			Context    context;
			const auto result = Run(context, {"howdy", "add", "-t"});
			ok &= Expect(result.status == 2 &&
			                 result.error.contains("tip: to pass '-t' as a value, use '-- -t'"),
			             "option-looking positional gets actionable double-dash tip");
		}
		{
			Context    context;
			const auto result = Run(context, {"howdy", "remove"});
			ok &= Expect(
			    result.status == 2 &&
			        result.error.contains(
			            "error: the following required arguments were not provided:\n  ID\n"),
			    "missing positional uses clap-style required-argument error");
		}
		{
			Context    context;
			const auto result = Run(context, {"howdy", "set", "device_path"});
			ok &= Expect(
			    result.status == 2 &&
			        result.error.contains(
			            "error: the following required arguments were not provided:\n  VALUE\n"),
			    "partially supplied positional reports only remaining required argument");
		}
		return ok;
	}

	auto TestCatalogDispatch() -> bool {
		bool        ok                  = true;
		std::size_t dispatched_commands = 0;
		for (const auto &command : CommandCatalog()) {
			if (command.kind != CommandKind::kEntrypoint) {
				continue;
			}
			++dispatched_commands;
			Context                  context;
			std::vector<std::string> arguments{"howdy"};
			if (command.user_target == howdy::native::UserTargetMode::kModelUser) {
				arguments.insert(arguments.end(), {"-U", "bob"});
			}
			arguments.emplace_back(command.name);
			switch (command.id) {
				case CommandId::kDisable:
					arguments.emplace_back("false");
					break;
				case CommandId::kRemove:
					arguments.emplace_back("0");
					break;
				case CommandId::kSet:
					arguments.insert(arguments.end(), {"key", "value"});
					break;
				default:
					break;
			}
			const auto result = Run(context, std::move(arguments));
			ok &= Expect(result.status == 0 && context.command_id == command.id,
			             "every catalog command maps to matching entrypoint");
		}
		ok &= Expect(dispatched_commands + 1 == CommandCatalog().size(),
		             "every production command is covered by dispatch test");
		return ok;
	}

	auto TestInvokingIdentityFailures() -> bool {
		bool ok = true;
		for (const auto status : {howdy::native::InvokingIdentityStatus::kInvalid,
		                          howdy::native::InvokingIdentityStatus::kConflicting}) {
			Context context;
			context.identity_status    = status;
			context.resolved_user      = "bob";
			const auto        result   = Run(context, {"howdy", "list"});
			const char *const expected = status == howdy::native::InvokingIdentityStatus::kInvalid
			                                 ? "Unable to determine the user: invalid "
			                                   "privilege-wrapper identity; please use "
			                                   "--user\n"
			                                 : "Unable to determine the user: conflicting "
			                                   "privilege-wrapper identity; please "
			                                   "use --user\n";
			ok &= Expect(result.status == 1 && result.output == expected,
			             "invalid invoking identity fails automatic model-user resolution");
			ok &= Expect(context.resolve_user_calls == 1 && context.effective_uid_calls == 1 &&
			                 !context.command_id.has_value() && context.command_arguments.empty(),
			             "invalid invoking identity never reaches command implementation");
		}

		Context context;
		context.identity_status = howdy::native::InvokingIdentityStatus::kNoWrapperIdentity;
		const auto result       = Run(context, {"howdy", "list"});
		ok &= Expect(result.status == 1 &&
		                 result.output == "Unable to determine the user; please use --user\n",
		             "direct root without explicit user is rejected");
		ok &= Expect(context.resolve_user_calls == 1 && context.effective_uid_calls == 1 &&
		                 !context.command_id.has_value() && context.command_arguments.empty(),
		             "direct root without explicit user does not dispatch");
		return ok;
	}

	auto TestExplicitRootUser() -> bool {
		bool ok = true;
		for (const auto &user_option : {std::string{"--user"}, std::string{"-U"}}) {
			Context context;
			context.command_result = 23;
			const auto result      = Run(context, {"howdy", user_option, "root", "list"});
			ok &= Expect(result.status == 23 && result.output == "command output\n" &&
			                 result.error == "command error\n",
			             "explicit root target dispatches model command");
			ok &= Expect(context.resolve_user_calls == 0,
			             "explicit root target skips default user resolution");
			ok &= Expect(context.command_id == CommandId::kList &&
			                 context.command_arguments ==
			                     std::vector<std::string>{"howdy-list", "root"},
			             "explicit root target is injected into list arguments");
		}
		return ok;
	}

}  // namespace

auto main() -> int {
	bool ok = true;

	{
		Context    context;
		const auto result = Run(context, {"howdy"});
		ok &= Expect(result.status == 0 &&
		                 result.output.starts_with("Usage: howdy [OPTIONS] <COMMAND>"),
		             "no command prints top-level help");
	}
	ok &= howdy::test::dispatch::RunHowdyCompletionTests();
	ok &= TestMissingCallback();
	{
		Context    context;
		const auto result = Run(context, {"howdy", "unknown"});
		ok &= Expect(result.status == 2 && result.output.empty() &&
		                 result.error.contains("unrecognized subcommand 'unknown'"),
		             "unknown command rejected as usage error");
	}
	ok &= TestUnknownCommandPrecedence();
	{
		Context context;
		context.command_result = 23;
		const auto result =
		    Run(context, {"howdy", "-U", "bob", "--plain", "-y", "add", "front-door"});
		ok &= Expect(result.status == 23, "command return code passed through");
		ok &= Expect(context.resolve_user_calls == 0,
		             "short user option skips default user resolution");
		ok &=
		    Expect(context.command_arguments ==
		               std::vector<std::string>{"howdy-add", "bob", "front-door", "--plain", "-y"},
		           "global options forwarded with user injection");
		ok &= Expect(result.output == "command output\n" && result.error == "command error\n",
		             "command output streams preserved");
	}
	{
		Context context;
		context.command_result = 23;
		context.resolved_user  = "root";
		const auto result      = Run(context, {"howdy", "config"});
		ok &= Expect(result.status == 23, "non-user command dispatched");
		ok &= Expect(context.resolve_user_calls == 0,
		             "non-user command skips target-user resolution");
		ok &= Expect(context.command_arguments == std::vector<std::string>{"howdy-config"},
		             "user injected only for model commands");
	}
	{
		Context context;
		context.command_result = 23;
		const auto result      = Run(context, {"howdy", "list"});
		ok &= Expect(result.status == 23, "default-user command return code passed through");
		ok &= Expect(context.resolve_user_calls == 1,
		             "default user resolved when user option is omitted");
		ok &= Expect(context.command_id == CommandId::kList, "list dispatched with default user");
		ok &= Expect(context.command_arguments == std::vector<std::string>{"howdy-list", "alice"},
		             "resolved default user injected into list arguments");
	}
	ok &= TestInvokingIdentityFailures();
	ok &= TestExplicitRootUser();
	{
		Context    context;
		const auto result = Run(context, {"howdy", "--user", "bob", "list"});
		ok &= Expect(result.status == 0, "long user option command dispatched");
		ok &= Expect(context.resolve_user_calls == 0,
		             "long user option skips default user resolution");
		ok &= Expect(context.command_id == CommandId::kList, "long user option dispatches list");
		ok &= Expect(context.command_arguments == std::vector<std::string>{"howdy-list", "bob"},
		             "long user option injected into list arguments");
	}
	ok &= TestEmptyUserOptions();
	{
		Context    context;
		const auto result = Run(context, {"howdy", "", "list"});
		ok &= Expect(result.status == 2 && result.output.empty() &&
		                 result.error.contains("unrecognized subcommand ''"),
		             "empty command token is not command absence");
		ok &= Expect(context.resolve_user_calls == 0 && context.effective_uid_calls == 0 &&
		                 !context.command_id.has_value(),
		             "empty command does not reinterpret later token");
	}
	{
		Context    context;
		const auto result = Run(context, {"howdy", "add", "--", "-y"});
		ok &= Expect(result.status == 0, "end-of-options literal label dispatches");
		ok &= Expect(context.command_arguments ==
		                 std::vector<std::string>{"howdy-add", "alice", "--", "-y"},
		             "end-of-options marker is forwarded to preserve literal label");
	}
	{
		Context context;
		context.effective_uid   = 1000;
		context.identity_status = howdy::native::InvokingIdentityStatus::kNoWrapperIdentity;
		const auto result       = Run(context, {"howdy", "add", "label; echo unsafe"});
		ok &=
		    Expect(result.status == 1 && result.output == "This command requires root privileges.\n"
		                                                  "Run it again with sudo.\n",
		           "non-root diagnostic is fixed and shell-safe");
		ok &= Expect(!result.output.contains("sudo howdy") &&
		                 !result.output.contains("label; echo unsafe"),
		             "non-root diagnostic does not reconstruct arbitrary argv");
		ok &= Expect(context.effective_uid_calls == 1 && context.resolve_user_calls == 0,
		             "non-root model command checks privileges before invoking identity");
		ok &= Expect(!context.command_id.has_value() && context.command_arguments.empty(),
		             "non-root model command is not dispatched");
	}
	{
		Context    context;
		const auto result = Run(context, {"howdy", "list", ""});
		ok &= Expect(result.status != 0 && !context.command_id.has_value(),
		             "surplus empty positional argument is rejected");
		ok &= Expect(context.resolve_user_calls == 0 && context.effective_uid_calls == 0,
		             "invalid positional syntax skips user and privilege work");
	}
	{
		Context    context;
		const auto result = Run(context, {"howdy", "-U"});
		ok &= Expect(result.status == 2, "trailing short user option rejected");
		ok &=
		    Expect(result.error.contains("-U <USER>") && result.error.contains("none was supplied"),
		           "trailing short user option reports missing argument");
		ok &= Expect(result.error.contains("Usage: howdy [OPTIONS] <COMMAND>"),
		             "trailing short user option does not print help");
		ok &= Expect(context.resolve_user_calls == 0 && context.effective_uid_calls == 0,
		             "trailing short user option skips user resolution and root check");
		ok &= Expect(!context.command_id.has_value() && context.command_arguments.empty(),
		             "trailing short user option does not dispatch command");
	}
	{
		Context    context;
		const auto result = Run(context, {"howdy", "--user"});
		ok &= Expect(result.status == 2, "trailing long user option rejected");
		ok &= Expect(result.error.contains("--user <USER>") &&
		                 result.error.contains("none was supplied"),
		             "trailing long user option reports missing argument");
		ok &= Expect(result.error.contains("Usage: howdy [OPTIONS] <COMMAND>"),
		             "trailing long user option does not print help");
		ok &= Expect(context.resolve_user_calls == 0 && context.effective_uid_calls == 0,
		             "trailing long user option skips user resolution and root check");
		ok &= Expect(!context.command_id.has_value() && context.command_arguments.empty(),
		             "trailing long user option does not dispatch command");
	}
	{
		Context    context;
		const auto result = Run(context, {"howdy", "list", "-U"});
		ok &= Expect(result.status == 2, "trailing short user option after command rejected");
		ok &=
		    Expect(result.error.contains("-U <USER>") && result.error.contains("none was supplied"),
		           "trailing short user option after command reports missing argument");
		ok &=
		    Expect(context.resolve_user_calls == 0 && context.effective_uid_calls == 0,
		           "trailing short user option after command skips user resolution and root check");
		ok &= Expect(!context.command_id.has_value() && context.command_arguments.empty(),
		             "trailing short user option after command does not dispatch command");
	}
	{
		Context    context;
		const auto result = Run(context, {"howdy", "add", "--user"});
		ok &= Expect(result.status == 2, "trailing long user option after command rejected");
		ok &= Expect(result.error.contains("--user <USER>") &&
		                 result.error.contains("none was supplied"),
		             "trailing long user option after command reports missing argument");
		ok &=
		    Expect(context.resolve_user_calls == 0 && context.effective_uid_calls == 0,
		           "trailing long user option after command skips user resolution and root check");
		ok &= Expect(!context.command_id.has_value() && context.command_arguments.empty(),
		             "trailing long user option after command does not dispatch command");
	}
	{
		Context context;
		context.effective_uid = 1000;
		const auto result     = Run(context, {"howdy", "list"});
		ok &=
		    Expect(result.status == 1 && result.output == "This command requires root privileges.\n"
		                                                  "Run it again with sudo.\n",
		           "root check runs before dispatch");
		ok &= Expect(context.command_arguments.empty(), "non-root command not dispatched");
	}
	ok &= TestClapStyleUsageErrors();
	ok &= TestStrictSyntaxCases();
	ok &= TestCatalogDispatch();

	return ok ? 0 : 1;
}
