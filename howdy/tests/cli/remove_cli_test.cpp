#include "cli/remove.hpp"
#include "cli/remove/internal.hpp"
#include "test_support.hpp"

#include <array>
#include <iostream>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

	using howdy::test::Expect;

	struct StreamRedirect {
		StreamRedirect(std::istream &input_stream, std::streambuf *new_input,
		               std::ostream &output_stream, std::streambuf *new_output)
		    : input(input_stream)
		    , output(output_stream)
		    , old_input(input_stream.rdbuf(new_input))
		    , old_output(output_stream.rdbuf(new_output)) {}

		~StreamRedirect() {
			input.rdbuf(old_input);
			output.rdbuf(old_output);
		}

		std::istream   &input;
		std::ostream   &output;
		std::streambuf *old_input;
		std::streambuf *old_output;
	};

	struct RemoveCliTestContext {
		howdy::native::UserModelListResult       list_result;
		howdy::native::UserModelMutationResult   remove_result;
		int                                      list_calls   = 0;
		int                                      remove_calls = 0;
		std::string                              listed_user;
		std::string                              removed_user;
		howdy::native::UserModelEntryExpectation expected;
	};

	auto ValidEntry() -> howdy::native::UserModelEntry {
		return {
		    .id      = 3,
		    .time    = 1234,
		    .label   = "front door",
		    .backend = "sface",
		    .metric  = howdy::native::FaceMetric::kCosine,
		    .model   = "face_recognition_sface_2021dec.onnx",
		};
	}

	auto SuccessContext() -> RemoveCliTestContext {
		RemoveCliTestContext context;
		context.list_result = {
		    .status  = howdy::native::UserModelStatus::kOk,
		    .entries = {ValidEntry()},
		};
		context.remove_result = {
		    .status = howdy::native::UserModelStatus::kOk,
		    .entry  = ValidEntry(),
		};
		return context;
	}

	auto ListCallback(void *raw_context, const std::string &user)
	    -> howdy::native::UserModelListResult {
		auto *context = static_cast<RemoveCliTestContext *>(raw_context);
		++context->list_calls;
		context->listed_user = user;
		return context->list_result;
	}

	auto RemoveCallback(void *raw_context, const std::string &user,
	                    const howdy::native::UserModelEntryExpectation &expected)
	    -> howdy::native::UserModelMutationResult {
		auto *context = static_cast<RemoveCliTestContext *>(raw_context);
		++context->remove_calls;
		context->removed_user = user;
		context->expected     = expected;
		return context->remove_result;
	}

	auto RunRemoveWithDependencies(
	    std::vector<std::string>                                  arguments,
	    const howdy::native::remove_internal::RemoveDependencies &dependencies,
	    const std::string &input = {}) -> std::pair<int, std::string> {
		std::vector<char *> argv;
		argv.reserve(arguments.size());
		for (auto &argument : arguments) {
			argv.push_back(argument.data());
		}

		std::istringstream input_stream(input);
		std::ostringstream output_stream;
		StreamRedirect redirect(std::cin, input_stream.rdbuf(), std::cout, output_stream.rdbuf());
		const int      result = howdy::native::remove_internal::RemoveMainWithDependencies(
		    static_cast<int>(argv.size()), argv.data(), dependencies);
		return {result, output_stream.str()};
	}

	auto RunRemove(RemoveCliTestContext &context, std::vector<std::string> arguments,
	               const std::string &input = {}) -> std::pair<int, std::string> {
		return RunRemoveWithDependencies(std::move(arguments),
		                                 {
		                                     .context                            = &context,
		                                     .list_user_model_entries            = ListCallback,
		                                     .remove_user_model_entry_if_matches = RemoveCallback,
		                                 },
		                                 input);
	}

	auto MissingUserReturnsWithoutCallbacks() -> bool {
		RemoveCliTestContext context;
		auto [result, output] = RunRemove(context, {"howdy-remove"});
		bool ok               = true;
		ok &= Expect(result == 1, "missing user returns 1");
		ok &= Expect(output.empty(), "missing user stays silent");
		ok &= Expect(context.list_calls == 0 && context.remove_calls == 0,
		             "missing user skips callbacks");
		return ok;
	}

	auto PublicMissingUserReturnsError() -> bool {
		auto                  command = std::to_array("howdy-remove");
		std::array<char *, 1> argv{command.data()};
		return Expect(RemoveMain(1, argv.data()) == 1, "public missing user returns status 1");
	}

	auto MissingModelIdPrintsGuidance() -> bool {
		auto context               = SuccessContext();
		auto [result, output]      = RunRemove(context, {"howdy-remove", "alice"});
		const std::string expected = "Please specify the model ID to remove.\n"
		                             "For example:\n\n\thowdy remove 0\n\n"
		                             "You can find the IDs by running:\n\n\thowdy list\n\n";
		return Expect(result == 1, "missing model ID returns 1") &&
		       Expect(output == expected, "missing model ID preserves guidance") &&
		       Expect(context.list_calls == 0 && context.remove_calls == 0,
		              "missing model ID skips callbacks");
	}

	auto ListStatusesPreserveMessages() -> bool {
		bool ok = true;
		for (const auto &[status, error, expected] :
		     std::vector<std::tuple<howdy::native::UserModelStatus, std::string, std::string>>{
		         {howdy::native::UserModelStatus::kNoModelDirectory,
		          {},
		          "No face models found. Please run:\n\n\thowdy add\n\n"},
		         {howdy::native::UserModelStatus::kNoModel,
		          {},
		          "No face models found. Please run:\n\n\thowdy add\n\n"},
		         {howdy::native::UserModelStatus::kParseError, "storage failed",
		          "storage failed\n"},
		     }) {
			auto context          = SuccessContext();
			context.list_result   = {.status = status, .error_message = error};
			auto [result, output] = RunRemove(context, {"howdy-remove", "alice", "3"});
			ok &= Expect(result == 1, "list failure returns 1");
			ok &= Expect(output == expected, "list failure preserves message");
			ok &= Expect(context.list_calls == 1 && context.remove_calls == 0,
			             "list failure skips removal");
		}
		return ok;
	}

	auto InvalidAndMissingIdsAbort() -> bool {
		bool ok = true;
		for (const std::string id : {"abc", "3x", "03", "4"}) {
			auto context          = SuccessContext();
			auto [result, output] = RunRemove(context, {"howdy-remove", "alice", id});
			ok &= Expect(result == 1, "invalid or missing ID returns 1");
			ok &= Expect(output == "No model with ID " + id + " exists for alice\n",
			             "invalid or missing ID preserves message");
			ok &= Expect(context.remove_calls == 0, "invalid or missing ID skips removal");
		}
		return ok;
	}

	auto RejectedConfirmationAborts() -> bool {
		auto context          = SuccessContext();
		auto [result, output] = RunRemove(context, {"howdy-remove", "alice", "3"}, "n\n");
		return Expect(result == 1, "rejected confirmation returns 1") &&
		       Expect(output == "Model \"front door\" will be removed for alice.\n"
		                        "Continue? [y/N]: \nNo confirmation received; aborting.\n",
		              "rejected confirmation preserves prompt and abort message") &&
		       Expect(context.remove_calls == 0, "rejected confirmation skips removal");
	}

	auto AcceptedConfirmationPassesCompleteExpectation() -> bool {
		auto context          = SuccessContext();
		auto [result, output] = RunRemove(context, {"howdy-remove", "alice", "3"}, "Y\n");
		const auto &expected  = context.expected;
		bool        ok        = true;
		ok &= Expect(result == 0, "accepted confirmation returns 0");
		ok &= Expect(context.list_calls == 1 && context.listed_user == "alice",
		             "accepted confirmation lists requested user");
		ok &= Expect(context.remove_calls == 1 && context.removed_user == "alice",
		             "accepted confirmation removes requested user model");
		ok &= Expect(expected.id == 3 && expected.time == 1234 && expected.label == "front door" &&
		                 expected.backend == "sface" &&
		                 expected.metric == howdy::native::FaceMetric::kCosine &&
		                 expected.model == "face_recognition_sface_2021dec.onnx",
		             "accepted confirmation passes complete stale-entry expectation");
		ok &= Expect(output == "Model \"front door\" will be removed for alice.\n"
		                       "Continue? [y/N]: \nRemoved model 3\n",
		             "accepted confirmation preserves full output");
		return ok;
	}

	auto IncompleteDependenciesAbortWithoutCallbacks() -> bool {
		auto context = SuccessContext();
		auto [null_list_result, null_list_output] =
		    RunRemoveWithDependencies({"howdy-remove", "alice", "3"},
		                              {
		                                  .context                            = &context,
		                                  .list_user_model_entries            = nullptr,
		                                  .remove_user_model_entry_if_matches = RemoveCallback,
		                              });

		bool ok = true;
		ok &= Expect(null_list_result == 1, "null list callback returns 1");
		ok &= Expect(null_list_output.empty(), "null list callback stays silent");
		ok &= Expect(context.list_calls == 0 && context.remove_calls == 0,
		             "null list callback skips storage callbacks");

		auto [null_remove_result, null_remove_output] = RunRemoveWithDependencies(
		    {"howdy-remove", "alice", "3"}, {
		                                        .context                            = &context,
		                                        .list_user_model_entries            = ListCallback,
		                                        .remove_user_model_entry_if_matches = nullptr,
		                                    });
		ok &= Expect(null_remove_result == 1, "null remove callback returns 1");
		ok &= Expect(null_remove_output.empty(), "null remove callback stays silent");
		ok &= Expect(context.list_calls == 0 && context.remove_calls == 0,
		             "null remove callback skips storage callbacks");
		return ok;
	}

	auto YesFlagBypassesConfirmation() -> bool {
		auto context          = SuccessContext();
		auto [result, output] = RunRemove(context, {"howdy-remove", "alice", "3", "-y"});
		return Expect(result == 0, "-y returns 0") &&
		       Expect(output == "Removed model 3\n", "-y skips confirmation prompt") &&
		       Expect(context.remove_calls == 1, "-y removes once");
	}

	auto RemoveFailureForwardsError() -> bool {
		auto context          = SuccessContext();
		context.remove_result = {
		    .status        = howdy::native::UserModelStatus::kModelChanged,
		    .error_message = "model changed",
		};
		auto [result, output] = RunRemove(context, {"howdy-remove", "alice", "3", "-y"});
		return Expect(result == 1, "stale removal returns 1") &&
		       Expect(output == "model changed\n", "stale removal forwards exact error") &&
		       Expect(context.remove_calls == 1, "stale removal called once");
	}

	auto LastModelSuccessPrintsDisabledMessage() -> bool {
		auto context                       = SuccessContext();
		context.remove_result.removed_last = true;
		auto [result, output] = RunRemove(context, {"howdy-remove", "alice", "3", "-y"});
		return Expect(result == 0, "last-model removal returns 0") &&
		       Expect(output ==
		                  "Removed final face model; face verification disabled for this user\n",
		              "last-model removal preserves disabled message");
	}

}  // namespace

auto main() -> int {
	bool ok = true;
	ok &= MissingUserReturnsWithoutCallbacks();
	ok &= PublicMissingUserReturnsError();
	ok &= MissingModelIdPrintsGuidance();
	ok &= ListStatusesPreserveMessages();
	ok &= InvalidAndMissingIdsAbort();
	ok &= RejectedConfirmationAborts();
	ok &= AcceptedConfirmationPassesCompleteExpectation();
	ok &= IncompleteDependenciesAbortWithoutCallbacks();
	ok &= YesFlagBypassesConfirmation();
	ok &= RemoveFailureForwardsError();
	ok &= LastModelSuccessPrintsDisabledMessage();
	return ok ? 0 : 1;
}
