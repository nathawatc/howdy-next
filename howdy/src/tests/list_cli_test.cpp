#include "cli/list_cli.hpp"
#include "cli/list_internal.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include <sys/wait.h>

namespace {

	struct StreamRedirect {
		StreamRedirect(std::ostream &stream, std::streambuf *new_output)
		    : output(stream)
		    , old_output(stream.rdbuf(new_output)) {}

		~StreamRedirect() {
			output.rdbuf(old_output);
		}

		std::ostream   &output;
		std::streambuf *old_output;
	};

	struct TimezoneGuard {
		TimezoneGuard() {
			if (const char *value = std::getenv("TZ"); value != nullptr) {
				old_timezone = value;
			}
			setenv("TZ", "UTC", 1);
			tzset();
		}

		~TimezoneGuard() {
			if (old_timezone.has_value()) {
				setenv("TZ", old_timezone->c_str(), 1);
			} else {
				unsetenv("TZ");
			}
			tzset();
		}

		std::optional<std::string> old_timezone;
	};

	struct ListCliTestContext {
		howdy::native::UserModelListResult list_result;
		int                                list_calls = 0;
		std::string                        listed_user;
	};

	auto expect(bool condition, const std::string &message) -> bool {
		if (!condition) {
			std::cerr << "FAIL: " << message << "\n";
			return false;
		}
		return true;
	}

	auto list_callback(void *raw_context, const std::string &user)
	    -> howdy::native::UserModelListResult {
		auto *context = static_cast<ListCliTestContext *>(raw_context);
		++context->list_calls;
		context->listed_user = user;
		return context->list_result;
	}

	auto
	run_list_with_dependencies(std::vector<std::string>                              arguments,
	                           const howdy::native::list_internal::ListDependencies &dependencies)
	    -> std::pair<int, std::string> {
		std::vector<char *> argv;
		argv.reserve(arguments.size());
		for (auto &argument : arguments) {
			argv.push_back(argument.data());
		}

		std::ostringstream output_stream;
		StreamRedirect     redirect(std::cout, output_stream.rdbuf());
		const int          result = howdy::native::list_internal::list_main_with_dependencies(
		    static_cast<int>(argv.size()), argv.data(), dependencies);
		return {result, output_stream.str()};
	}

	auto run_list(ListCliTestContext &context, std::vector<std::string> arguments)
	    -> std::pair<int, std::string> {
		return run_list_with_dependencies(
		    std::move(arguments), {.context = &context, .list_user_model_entries = list_callback});
	}

	auto successful_entries() -> std::vector<howdy::native::UserModelEntry> {
		return {
		    {.id = 3, .time = 0, .label = "front door"},
		    {.id = 12, .time = 0, .label = "desk"},
		};
	}

	auto internal_missing_user_returns_without_callback() -> bool {
		ListCliTestContext context;
		auto [result, output] = run_list(context, {"howdy-list"});
		return expect(result == 1, "internal missing user returns 1") &&
		       expect(output.empty(), "internal missing user stays silent") &&
		       expect(context.list_calls == 0, "internal missing user skips callback");
	}

	auto public_missing_user_terminates_process() -> bool {
		const pid_t child_pid = fork();
		if (!expect(child_pid >= 0, "fork public list child")) {
			return false;
		}
		if (child_pid == 0) {
			auto                  command = std::to_array("howdy-list");
			std::array<char *, 1> argv{command.data()};
			list_main(1, argv.data());
			_exit(42);
		}

		int status = 0;
		if (!expect(waitpid(child_pid, &status, 0) == child_pid, "wait for public list child")) {
			return false;
		}
		return expect(WIFEXITED(status) && WEXITSTATUS(status) == 1,
		              "public missing user terminates with status 1");
	}

	auto null_dependency_aborts_silently() -> bool {
		auto [result, output] = run_list_with_dependencies({"howdy-list", "alice"},
		                                                   {.list_user_model_entries = nullptr});
		return expect(result == 1, "null dependency returns 1") &&
		       expect(output.empty(), "null dependency stays silent");
	}

	auto no_model_directory_prints_guidance() -> bool {
		ListCliTestContext context;
		context.list_result   = {.status = howdy::native::UserModelStatus::kNoModelDirectory};
		auto [result, output] = run_list(context, {"howdy-list", "alice"});
		return expect(result == 1, "no model directory returns 1") &&
		       expect(output == "Face models have not been initialized yet, please run:\n"
		                        "\n\tsudo howdy -U alice add\n\n",
		              "no model directory preserves guidance") &&
		       expect(context.list_calls == 1 && context.listed_user == "alice",
		              "no model directory lists alice once");
	}

	auto no_model_preserves_normal_and_plain_output() -> bool {
		ListCliTestContext context;
		context.list_result                 = {.status = howdy::native::UserModelStatus::kNoModel};
		auto [normal_result, normal_output] = run_list(context, {"howdy-list", "alice"});
		auto [plain_result, plain_output]   = run_list(context, {"howdy-list", "alice", "--plain"});
		return expect(normal_result == 1, "no model normal returns 1") &&
		       expect(normal_output == "No face model known for the user alice, please run:\n"
		                               "\n\tsudo howdy -U alice add\n\n",
		              "no model normal preserves guidance") &&
		       expect(plain_result == 1, "no model plain returns 1") &&
		       expect(plain_output.empty(), "no model plain stays silent");
	}

	auto storage_failure_preserves_normal_and_plain_output() -> bool {
		ListCliTestContext context;
		context.list_result = {
		    .status        = howdy::native::UserModelStatus::kParseError,
		    .error_message = "storage failed",
		};
		auto [normal_result, normal_output] = run_list(context, {"howdy-list", "alice"});
		auto [plain_result, plain_output]   = run_list(context, {"howdy-list", "alice", "--plain"});
		return expect(normal_result == 1, "storage failure normal returns 1") &&
		       expect(normal_output == "storage failed\n", "storage failure prints error") &&
		       expect(plain_result == 1, "storage failure plain returns 1") &&
		       expect(plain_output.empty(), "storage failure plain stays silent");
	}

	auto successful_output_preserves_formats() -> bool {
		ListCliTestContext context;
		context.list_result = {
		    .status  = howdy::native::UserModelStatus::kOk,
		    .entries = successful_entries(),
		};
		auto [normal_result, normal_output] = run_list(context, {"howdy-list", "alice"});
		auto [plain_result, plain_output]   = run_list(context, {"howdy-list", "alice", "--plain"});
		return expect(normal_result == 0, "normal success returns 0") &&
		       expect(normal_output == "3   1970-01-01 00:00:00  front door\n"
		                               "12  1970-01-01 00:00:00  desk\n\n",
		              "normal success preserves output") &&
		       expect(plain_result == 0, "plain success returns 0") &&
		       expect(plain_output == "3,1970-01-01 00:00:00,front door\n"
		                              "12,1970-01-01 00:00:00,desk\n\n",
		              "plain success preserves CSV output");
	}

	auto plain_position_and_unknown_arguments_are_preserved() -> bool {
		ListCliTestContext context;
		context.list_result = {
		    .status  = howdy::native::UserModelStatus::kOk,
		    .entries = {{.id = 3, .time = 0, .label = "front door"}},
		};
		auto [result, output] = run_list(context, {"howdy-list", "alice", "ignored", "--plain"});
		return expect(result == 0, "positioned plain with unknown argument succeeds") &&
		       expect(output == "3,1970-01-01 00:00:00,front door\n\n",
		              "positioned plain preserves output");
	}

	auto zero_entries_prints_final_newline() -> bool {
		ListCliTestContext context;
		context.list_result   = {.status = howdy::native::UserModelStatus::kOk};
		auto [result, output] = run_list(context, {"howdy-list", "alice"});
		return expect(result == 0, "zero entries returns 0") &&
		       expect(output == "\n", "zero entries prints one newline");
	}

}  // namespace

auto main() -> int {
	const TimezoneGuard timezone_guard;
	bool                ok = true;
	ok &= internal_missing_user_returns_without_callback();
	ok &= public_missing_user_terminates_process();
	ok &= null_dependency_aborts_silently();
	ok &= no_model_directory_prints_guidance();
	ok &= no_model_preserves_normal_and_plain_output();
	ok &= storage_failure_preserves_normal_and_plain_output();
	ok &= successful_output_preserves_formats();
	ok &= plain_position_and_unknown_arguments_are_preserved();
	ok &= zero_entries_prints_final_newline();
	return ok ? 0 : 1;
}
