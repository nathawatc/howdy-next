#include "cli/remove_cli.hpp"
#include "cli/remove_internal.hpp"
#include "test_support.hpp"

#include <array>
#include <iostream>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

	using howdy::test::expect;

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

	auto valid_entry() -> howdy::native::UserModelEntry {
		return {
		    .id      = 3,
		    .time    = 1234,
		    .label   = "front door",
		    .backend = "sface",
		    .metric  = "cosine",
		    .model   = "face_recognition_sface_2021dec.onnx",
		};
	}

	auto success_context() -> RemoveCliTestContext {
		RemoveCliTestContext context;
		context.list_result = {
		    .status  = howdy::native::UserModelStatus::kOk,
		    .entries = {valid_entry()},
		};
		context.remove_result = {
		    .status = howdy::native::UserModelStatus::kOk,
		    .entry  = valid_entry(),
		};
		return context;
	}

	auto list_callback(void *raw_context, const std::string &user)
	    -> howdy::native::UserModelListResult {
		auto *context = static_cast<RemoveCliTestContext *>(raw_context);
		++context->list_calls;
		context->listed_user = user;
		return context->list_result;
	}

	auto remove_callback(void *raw_context, const std::string &user,
	                     const howdy::native::UserModelEntryExpectation &expected)
	    -> howdy::native::UserModelMutationResult {
		auto *context = static_cast<RemoveCliTestContext *>(raw_context);
		++context->remove_calls;
		context->removed_user = user;
		context->expected     = expected;
		return context->remove_result;
	}

	auto run_remove_with_dependencies(
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
		const int      result = howdy::native::remove_internal::remove_main_with_dependencies(
		    static_cast<int>(argv.size()), argv.data(), dependencies);
		return {result, output_stream.str()};
	}

	auto run_remove(RemoveCliTestContext &context, std::vector<std::string> arguments,
	                const std::string &input = {}) -> std::pair<int, std::string> {
		return run_remove_with_dependencies(
		    std::move(arguments),
		    {
		        .context                            = &context,
		        .list_user_model_entries            = list_callback,
		        .remove_user_model_entry_if_matches = remove_callback,
		    },
		    input);
	}

	auto missing_user_returns_without_callbacks() -> bool {
		RemoveCliTestContext context;
		auto [result, output] = run_remove(context, {"howdy-remove"});
		bool ok               = true;
		ok &= expect(result == 1, "missing user returns 1");
		ok &= expect(output.empty(), "missing user stays silent");
		ok &= expect(context.list_calls == 0 && context.remove_calls == 0,
		             "missing user skips callbacks");
		return ok;
	}

	auto public_missing_user_returns_error() -> bool {
		auto                  command = std::to_array("howdy-remove");
		std::array<char *, 1> argv{command.data()};
		return expect(remove_main(1, argv.data()) == 1, "public missing user returns status 1");
	}

	auto missing_model_id_prints_guidance() -> bool {
		auto context               = success_context();
		auto [result, output]      = run_remove(context, {"howdy-remove", "alice"});
		const std::string expected = "Please specify the model ID to remove.\n"
		                             "For example:\n\n\thowdy remove 0\n\n"
		                             "You can find the IDs by running:\n\n\thowdy list\n\n";
		return expect(result == 1, "missing model ID returns 1") &&
		       expect(output == expected, "missing model ID preserves guidance") &&
		       expect(context.list_calls == 0 && context.remove_calls == 0,
		              "missing model ID skips callbacks");
	}

	auto list_statuses_preserve_messages() -> bool {
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
			auto context          = success_context();
			context.list_result   = {.status = status, .error_message = error};
			auto [result, output] = run_remove(context, {"howdy-remove", "alice", "3"});
			ok &= expect(result == 1, "list failure returns 1");
			ok &= expect(output == expected, "list failure preserves message");
			ok &= expect(context.list_calls == 1 && context.remove_calls == 0,
			             "list failure skips removal");
		}
		return ok;
	}

	auto invalid_and_missing_ids_abort() -> bool {
		bool ok = true;
		for (const std::string id : {"abc", "3x", "03", "4"}) {
			auto context          = success_context();
			auto [result, output] = run_remove(context, {"howdy-remove", "alice", id});
			ok &= expect(result == 1, "invalid or missing ID returns 1");
			ok &= expect(output == "No model with ID " + id + " exists for alice\n",
			             "invalid or missing ID preserves message");
			ok &= expect(context.remove_calls == 0, "invalid or missing ID skips removal");
		}
		return ok;
	}

	auto rejected_confirmation_aborts() -> bool {
		auto context          = success_context();
		auto [result, output] = run_remove(context, {"howdy-remove", "alice", "3"}, "n\n");
		return expect(result == 1, "rejected confirmation returns 1") &&
		       expect(output == "Model \"front door\" will be removed for alice.\n"
		                        "Continue? [y/N]: \nNo confirmation received; aborting.\n",
		              "rejected confirmation preserves prompt and abort message") &&
		       expect(context.remove_calls == 0, "rejected confirmation skips removal");
	}

	auto accepted_confirmation_passes_complete_expectation() -> bool {
		auto context          = success_context();
		auto [result, output] = run_remove(context, {"howdy-remove", "alice", "3"}, "Y\n");
		const auto &expected  = context.expected;
		bool        ok        = true;
		ok &= expect(result == 0, "accepted confirmation returns 0");
		ok &= expect(context.list_calls == 1 && context.listed_user == "alice",
		             "accepted confirmation lists requested user");
		ok &= expect(context.remove_calls == 1 && context.removed_user == "alice",
		             "accepted confirmation removes requested user model");
		ok &= expect(expected.id == 3 && expected.time == 1234 && expected.label == "front door" &&
		                 expected.backend == "sface" && expected.metric == "cosine" &&
		                 expected.model == "face_recognition_sface_2021dec.onnx",
		             "accepted confirmation passes complete stale-entry expectation");
		ok &= expect(output == "Model \"front door\" will be removed for alice.\n"
		                       "Continue? [y/N]: \nRemoved model 3\n",
		             "accepted confirmation preserves full output");
		return ok;
	}

	auto incomplete_dependencies_abort_without_callbacks() -> bool {
		auto context = success_context();
		auto [null_list_result, null_list_output] =
		    run_remove_with_dependencies({"howdy-remove", "alice", "3"},
		                                 {
		                                     .context                            = &context,
		                                     .list_user_model_entries            = nullptr,
		                                     .remove_user_model_entry_if_matches = remove_callback,
		                                 });

		bool ok = true;
		ok &= expect(null_list_result == 1, "null list callback returns 1");
		ok &= expect(null_list_output.empty(), "null list callback stays silent");
		ok &= expect(context.list_calls == 0 && context.remove_calls == 0,
		             "null list callback skips storage callbacks");

		auto [null_remove_result, null_remove_output] = run_remove_with_dependencies(
		    {"howdy-remove", "alice", "3"}, {
		                                        .context                            = &context,
		                                        .list_user_model_entries            = list_callback,
		                                        .remove_user_model_entry_if_matches = nullptr,
		                                    });
		ok &= expect(null_remove_result == 1, "null remove callback returns 1");
		ok &= expect(null_remove_output.empty(), "null remove callback stays silent");
		ok &= expect(context.list_calls == 0 && context.remove_calls == 0,
		             "null remove callback skips storage callbacks");
		return ok;
	}

	auto yes_flag_bypasses_confirmation() -> bool {
		auto context          = success_context();
		auto [result, output] = run_remove(context, {"howdy-remove", "alice", "3", "-y"});
		return expect(result == 0, "-y returns 0") &&
		       expect(output == "Removed model 3\n", "-y skips confirmation prompt") &&
		       expect(context.remove_calls == 1, "-y removes once");
	}

	auto remove_failure_forwards_error() -> bool {
		auto context          = success_context();
		context.remove_result = {
		    .status        = howdy::native::UserModelStatus::kModelChanged,
		    .error_message = "model changed",
		};
		auto [result, output] = run_remove(context, {"howdy-remove", "alice", "3", "-y"});
		return expect(result == 1, "stale removal returns 1") &&
		       expect(output == "model changed\n", "stale removal forwards exact error") &&
		       expect(context.remove_calls == 1, "stale removal called once");
	}

	auto last_model_success_prints_disabled_message() -> bool {
		auto context                       = success_context();
		context.remove_result.removed_last = true;
		auto [result, output] = run_remove(context, {"howdy-remove", "alice", "3", "-y"});
		return expect(result == 0, "last-model removal returns 0") &&
		       expect(output ==
		                  "Removed final face model; face verification disabled for this user\n",
		              "last-model removal preserves disabled message");
	}

}  // namespace

auto main() -> int {
	bool ok = true;
	ok &= missing_user_returns_without_callbacks();
	ok &= public_missing_user_returns_error();
	ok &= missing_model_id_prints_guidance();
	ok &= list_statuses_preserve_messages();
	ok &= invalid_and_missing_ids_abort();
	ok &= rejected_confirmation_aborts();
	ok &= accepted_confirmation_passes_complete_expectation();
	ok &= incomplete_dependencies_abort_without_callbacks();
	ok &= yes_flag_bypasses_confirmation();
	ok &= remove_failure_forwards_error();
	ok &= last_model_success_prints_disabled_message();
	return ok ? 0 : 1;
}
