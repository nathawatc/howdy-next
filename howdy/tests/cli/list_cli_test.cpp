
#include "cli/list/internal.hpp"
#include "test_support.hpp"

#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

	using howdy::test::Expect;

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

	auto ListCallback(void *raw_context, const std::string &user)
	    -> howdy::native::UserModelListResult {
		auto *context = static_cast<ListCliTestContext *>(raw_context);
		++context->list_calls;
		context->listed_user = user;
		return context->list_result;
	}

	auto RunListWithDependencies(const howdy::native::CommandInvocation               &invocation,
	                             const howdy::native::list_internal::ListDependencies &dependencies)
	    -> std::pair<int, std::string> {
		std::ostringstream output_stream;
		StreamRedirect     redirect(std::cout, output_stream.rdbuf());
		const int          result =
		    howdy::native::list_internal::ListMainWithDependencies(invocation, dependencies);
		return {result, output_stream.str()};
	}

	auto RunList(ListCliTestContext &context, const howdy::native::CommandInvocation &invocation)
	    -> std::pair<int, std::string> {
		return RunListWithDependencies(
		    invocation, {.context = &context, .list_user_model_entries = ListCallback});
	}

	auto SuccessfulEntries() -> std::vector<howdy::native::UserModelEntry> {
		return {
		    {.id = 3, .time = 0, .label = "front door"},
		    {.id = 12, .time = 0, .label = "desk"},
		};
	}

	auto NullDependencyAbortsSilently() -> bool {
		auto [result, output] = RunListWithDependencies({.resolved_user = "alice"},
		                                                {.list_user_model_entries = nullptr});
		return Expect(result == 1, "null dependency returns 1") &&
		       Expect(output.empty(), "null dependency stays silent");
	}

	auto NoModelDirectoryPrintsGuidance() -> bool {
		ListCliTestContext context;
		context.list_result   = {.status = howdy::native::UserModelStatus::kNoModelDirectory};
		auto [result, output] = RunList(context, {.resolved_user = "alice"});
		return Expect(result == 1, "no model directory returns 1") &&
		       Expect(output == "No face models found. Use the add command to add a face model for "
		                        "this user.\n",
		              "no model directory prints safe guidance") &&
		       Expect(context.list_calls == 1 && context.listed_user == "alice",
		              "no model directory lists alice once");
	}

	auto NoModelPreservesNormalAndPlainOutput() -> bool {
		ListCliTestContext context;
		context.list_result                 = {.status = howdy::native::UserModelStatus::kNoModel};
		auto [normal_result, normal_output] = RunList(context, {.resolved_user = "alice"});
		auto [plain_result, plain_output] =
		    RunList(context, {.resolved_user = "alice", .plain = true});
		return Expect(normal_result == 1, "no model normal returns 1") &&
		       Expect(normal_output == "No face models found. Use the add command to add a face "
		                               "model for this user.\n",
		              "no model normal prints safe guidance") &&
		       Expect(plain_result == 1, "no model plain returns 1") &&
		       Expect(plain_output.empty(), "no model plain stays silent");
	}

	auto NoModelGuidanceDoesNotRenderUserAsShellCommand() -> bool {
		ListCliTestContext context;
		context.list_result   = {.status = howdy::native::UserModelStatus::kNoModelDirectory};
		auto [result, output] = RunList(context, {.resolved_user = "$(id)"});
		return Expect(result == 1, "shell-like user no model directory returns 1") &&
		       Expect(output == "No face models found. Use the add command to add a face model for "
		                        "this user.\n",
		              "no model guidance does not render user into a shell command") &&
		       Expect(context.list_calls == 1 && context.listed_user == "$(id)",
		              "shell-like user is passed only to model lookup");
	}

	auto StorageFailurePreservesNormalAndPlainOutput() -> bool {
		ListCliTestContext context;
		context.list_result = {
		    .status        = howdy::native::UserModelStatus::kParseError,
		    .error_message = "storage failed",
		};
		auto [normal_result, normal_output] = RunList(context, {.resolved_user = "alice"});
		auto [plain_result, plain_output] =
		    RunList(context, {.resolved_user = "alice", .plain = true});
		return Expect(normal_result == 1, "storage failure normal returns 1") &&
		       Expect(normal_output == "storage failed\n", "storage failure prints error") &&
		       Expect(plain_result == 1, "storage failure plain returns 1") &&
		       Expect(plain_output.empty(), "storage failure plain stays silent");
	}

	auto SuccessfulOutputPreservesFormats() -> bool {
		ListCliTestContext context;
		context.list_result = {
		    .status  = howdy::native::UserModelStatus::kOk,
		    .entries = SuccessfulEntries(),
		};
		auto [normal_result, normal_output] = RunList(context, {.resolved_user = "alice"});
		auto [plain_result, plain_output] =
		    RunList(context, {.resolved_user = "alice", .plain = true});
		return Expect(normal_result == 0, "normal success returns 0") &&
		       Expect(normal_output == "3   1970-01-01 00:00:00  front door\n"
		                               "12  1970-01-01 00:00:00  desk\n\n",
		              "normal success preserves output") &&
		       Expect(plain_result == 0, "plain success returns 0") &&
		       Expect(plain_output == "3,1970-01-01 00:00:00,front door\n"
		                              "12,1970-01-01 00:00:00,desk\n\n",
		              "plain success preserves CSV output");
	}

	auto OutOfRangeTimestampEmitsFallback() -> bool {
		ListCliTestContext context;
		context.list_result = {
		    .status  = howdy::native::UserModelStatus::kOk,
		    .entries = {{.id    = 3,
		                 .time  = std::numeric_limits<long long>::max(),
		                 .label = "front door"}},
		};
		auto [normal_result, normal_output] = RunList(context, {.resolved_user = "alice"});
		auto [plain_result, plain_output] =
		    RunList(context, {.resolved_user = "alice", .plain = true});
		return Expect(normal_result == 0, "out-of-range normal returns 0") &&
		       Expect(normal_output == "3   invalid-time  front door\n\n",
		              "out-of-range normal uses fallback") &&
		       Expect(plain_result == 0, "out-of-range plain returns 0") &&
		       Expect(plain_output == "3,invalid-time,front door\n\n",
		              "out-of-range plain uses fallback");
	}

	auto PlainOutputEscapesCsvFields() -> bool {
		ListCliTestContext context;
		context.list_result = {
		    .status  = howdy::native::UserModelStatus::kOk,
		    .entries = {{.id = 3, .time = 0, .label = "front,door"},
		                {.id = 12, .time = 0, .label = "quote\"door"},
		                {.id = 13, .time = 0, .label = "front,\"door"}},
		};
		auto [result, output] = RunList(context, {.resolved_user = "alice", .plain = true});
		return Expect(result == 0, "CSV-character labels list successfully") &&
		       Expect(output == "3,1970-01-01 00:00:00,\"front,door\"\n"
		                        "12,1970-01-01 00:00:00,\"quote\"\"door\"\n"
		                        "13,1970-01-01 00:00:00,\"front,\"\"door\"\n\n",
		              "plain output escapes comma and quote fields as CSV");
	}

	auto ZeroEntriesPrintsFinalNewline() -> bool {
		ListCliTestContext context;
		context.list_result   = {.status = howdy::native::UserModelStatus::kOk};
		auto [result, output] = RunList(context, {.resolved_user = "alice"});
		return Expect(result == 0, "zero entries returns 0") &&
		       Expect(output == "\n", "zero entries prints one newline");
	}

}  // namespace

auto main() -> int {
	const TimezoneGuard timezone_guard;
	bool                ok = true;
	ok &= NullDependencyAbortsSilently();
	ok &= NoModelDirectoryPrintsGuidance();
	ok &= NoModelPreservesNormalAndPlainOutput();
	ok &= NoModelGuidanceDoesNotRenderUserAsShellCommand();
	ok &= StorageFailurePreservesNormalAndPlainOutput();
	ok &= SuccessfulOutputPreservesFormats();
	ok &= OutOfRangeTimestampEmitsFallback();
	ok &= PlainOutputEscapesCsvFields();
	ok &= ZeroEntriesPrintsFinalNewline();
	return ok ? 0 : 1;
}
