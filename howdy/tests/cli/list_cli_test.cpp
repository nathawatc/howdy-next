#include "cli/list_cli.hpp"
#include "cli/list_internal.hpp"
#include "test_support.hpp"

#include <array>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

	using howdy::test::expect;

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

	auto public_missing_user_returns_error() -> bool {
		auto                  command = std::to_array("howdy-list");
		std::array<char *, 1> argv{command.data()};
		return expect(list_main(1, argv.data()) == 1, "public missing user returns status 1");
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
		       expect(output == "No face models found. Use the add command to add a face model for "
		                        "this user.\n",
		              "no model directory prints safe guidance") &&
		       expect(context.list_calls == 1 && context.listed_user == "alice",
		              "no model directory lists alice once");
	}

	auto no_model_preserves_normal_and_plain_output() -> bool {
		ListCliTestContext context;
		context.list_result                 = {.status = howdy::native::UserModelStatus::kNoModel};
		auto [normal_result, normal_output] = run_list(context, {"howdy-list", "alice"});
		auto [plain_result, plain_output]   = run_list(context, {"howdy-list", "alice", "--plain"});
		return expect(normal_result == 1, "no model normal returns 1") &&
		       expect(normal_output == "No face models found. Use the add command to add a face "
		                               "model for this user.\n",
		              "no model normal prints safe guidance") &&
		       expect(plain_result == 1, "no model plain returns 1") &&
		       expect(plain_output.empty(), "no model plain stays silent");
	}

	auto no_model_guidance_does_not_render_user_as_shell_command() -> bool {
		ListCliTestContext context;
		context.list_result   = {.status = howdy::native::UserModelStatus::kNoModelDirectory};
		auto [result, output] = run_list(context, {"howdy-list", "$(id)"});
		return expect(result == 1, "shell-like user no model directory returns 1") &&
		       expect(output == "No face models found. Use the add command to add a face model for "
		                        "this user.\n",
		              "no model guidance does not render user into a shell command") &&
		       expect(context.list_calls == 1 && context.listed_user == "$(id)",
		              "shell-like user is passed only to model lookup");
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

	auto out_of_range_timestamp_emits_fallback() -> bool {
		ListCliTestContext context;
		context.list_result = {
		    .status  = howdy::native::UserModelStatus::kOk,
		    .entries = {{.id    = 3,
		                 .time  = std::numeric_limits<long long>::max(),
		                 .label = "front door"}},
		};
		auto [normal_result, normal_output] = run_list(context, {"howdy-list", "alice"});
		auto [plain_result, plain_output]   = run_list(context, {"howdy-list", "alice", "--plain"});
		return expect(normal_result == 0, "out-of-range normal returns 0") &&
		       expect(normal_output == "3   invalid-time  front door\n\n",
		              "out-of-range normal uses fallback") &&
		       expect(plain_result == 0, "out-of-range plain returns 0") &&
		       expect(plain_output == "3,invalid-time,front door\n\n",
		              "out-of-range plain uses fallback");
	}

	auto plain_unknown_argument_is_rejected_before_listing() -> bool {
		ListCliTestContext context;
		context.list_result = {
		    .status  = howdy::native::UserModelStatus::kOk,
		    .entries = {{.id = 3, .time = 0, .label = "front door"}},
		};
		auto [result, output] = run_list(context, {"howdy-list", "alice", "ignored", "--plain"});
		return expect(result == 1, "unknown list argument is rejected") &&
		       expect(output.empty(), "unknown list argument has no output") &&
		       expect(context.list_calls == 0, "unknown list argument skips listing");
	}

	auto plain_output_escapes_csv_fields() -> bool {
		ListCliTestContext context;
		context.list_result = {
		    .status  = howdy::native::UserModelStatus::kOk,
		    .entries = {{.id = 3, .time = 0, .label = "front,door"},
		                {.id = 12, .time = 0, .label = "quote\"door"},
		                {.id = 13, .time = 0, .label = "front,\"door"}},
		};
		auto [result, output] = run_list(context, {"howdy-list", "alice", "--plain"});
		return expect(result == 0, "CSV-character labels list successfully") &&
		       expect(output == "3,1970-01-01 00:00:00,\"front,door\"\n"
		                        "12,1970-01-01 00:00:00,\"quote\"\"door\"\n"
		                        "13,1970-01-01 00:00:00,\"front,\"\"door\"\n\n",
		              "plain output escapes comma and quote fields as CSV");
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
	ok &= public_missing_user_returns_error();
	ok &= null_dependency_aborts_silently();
	ok &= no_model_directory_prints_guidance();
	ok &= no_model_preserves_normal_and_plain_output();
	ok &= no_model_guidance_does_not_render_user_as_shell_command();
	ok &= storage_failure_preserves_normal_and_plain_output();
	ok &= successful_output_preserves_formats();
	ok &= out_of_range_timestamp_emits_fallback();
	ok &= plain_unknown_argument_is_rejected_before_listing();
	ok &= plain_output_escapes_csv_fields();
	ok &= zero_entries_prints_final_newline();
	return ok ? 0 : 1;
}
