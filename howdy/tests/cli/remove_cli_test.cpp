
#include "app/command_invocation.hpp"
#include "auth_helper/staged_copies.hpp"
#include "cli/remove/internal.hpp"
#include "storage/user_model_status.hpp"
#include "storage/user_model_types.hpp"
#include "support/face_metric.hpp"
#include "test_support.hpp"

#include <iostream>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

	using howdy::test::Expect;
	using howdy::test::StreamRedirect;

	struct RemoveCliTestContext {
		howdy::native::UserModelListResult             list_result;
		howdy::native::UserModelMutationResult         remove_result;
		int                                            list_calls   = 0;
		int                                            remove_calls = 0;
		int                                            purge_calls  = 0;
		std::string                                    purged_user;
		howdy::native::auth_helper::StagedPurgeTrigger purge_trigger =
		    howdy::native::auth_helper::StagedPurgeTrigger::kModelsChanged;
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

	auto PurgeCallback(void *raw_context, const std::string &user,
	                   howdy::native::auth_helper::StagedPurgeTrigger trigger) -> void {
		auto *context = static_cast<RemoveCliTestContext *>(raw_context);
		++context->purge_calls;
		context->purged_user   = user;
		context->purge_trigger = trigger;
	}

	auto RunRemoveWithDependencies(
	    const howdy::native::CommandInvocation                   &invocation,
	    const howdy::native::remove_internal::RemoveDependencies &dependencies,
	    const std::string &input = {}) -> std::pair<int, std::string> {
		std::istringstream const input_stream(input);
		std::ostringstream const output_stream;
		StreamRedirect const     redirect(std::cin, input_stream.rdbuf(), std::cout,
		                                  output_stream.rdbuf());
		const int                result =
		    howdy::native::remove_internal::RemoveMainWithDependencies(invocation, dependencies);
		return {result, output_stream.str()};
	}

	auto RunRemove(RemoveCliTestContext                   &context,
	               const howdy::native::CommandInvocation &invocation,
	               const std::string &input = {}) -> std::pair<int, std::string> {
		return RunRemoveWithDependencies(invocation,
		                                 {
		                                     .context                            = &context,
		                                     .list_user_model_entries            = ListCallback,
		                                     .remove_user_model_entry_if_matches = RemoveCallback,
		                                     .purge_staged_copies                = PurgeCallback,
		                                 },
		                                 input);
	}

	auto ListStatusesPreserveMessages() -> bool {
		bool ok = true;
		for (const auto &[status, error, expected] :
		     std::vector<std::tuple<howdy::native::UserModelStatus, std::string, std::string>>{
		         {
		             howdy::native::UserModelStatus::kNoModelDirectory,
		             {},
		             "No face models found. Please run:\n\n\thowdy add\n\n",
		         },
		         {
		             howdy::native::UserModelStatus::kNoModel,
		             {},
		             "No face models found. Please run:\n\n\thowdy add\n\n",
		         },
		         {
		             howdy::native::UserModelStatus::kParseError,
		             "storage failed",
		             "storage failed\n",
		         },
		     }) {
			auto context        = SuccessContext();
			context.list_result = {.status = status, .error_message = error};
			auto [result, output] =
			    RunRemove(context, {.resolved_user = "alice", .positionals = {"3"}});
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
			auto context = SuccessContext();
			auto [result, output] =
			    RunRemove(context, {.resolved_user = "alice", .positionals = {id}});
			ok &= Expect(result == 1, "invalid or missing ID returns 1");
			ok &= Expect(output == "No model with ID " + id + " exists for alice\n",
			             "invalid or missing ID preserves message");
			ok &= Expect(context.remove_calls == 0, "invalid or missing ID skips removal");
		}
		return ok;
	}

	auto MissingUserOrPositionalAbortsBeforeStorage() -> bool {
		bool ok = true;
		for (const auto &invocation : {
		         howdy::native::CommandInvocation{.positionals = {"3"}, .assume_yes = true},
		         howdy::native::CommandInvocation{.resolved_user = "alice", .assume_yes = true},
		     }) {
			auto context          = SuccessContext();
			auto [result, output] = RunRemove(context, invocation);
			ok &= Expect(result == 1, "missing user or ID returns 1");
			ok &= Expect(output.empty(), "missing user or ID stays silent");
			ok &= Expect(context.list_calls == 0 && context.remove_calls == 0,
			             "missing user or ID skips storage callbacks");
		}
		return ok;
	}

	auto RejectedConfirmationAborts() -> bool {
		auto context = SuccessContext();
		auto [result, output] =
		    RunRemove(context, {.resolved_user = "alice", .positionals = {"3"}}, "n\n");
		return Expect(result == 1, "rejected confirmation returns 1") &&
		       Expect(output == "Model \"front door\" will be removed for alice.\n"
		                        "Continue? [y/N]: \nNo confirmation received; aborting.\n",
		              "rejected confirmation preserves prompt and abort message") &&
		       Expect(context.remove_calls == 0, "rejected confirmation skips removal");
	}

	auto AcceptedConfirmationPassesCompleteExpectation() -> bool {
		auto context = SuccessContext();
		auto [result, output] =
		    RunRemove(context, {.resolved_user = "alice", .positionals = {"3"}}, "Y\n");
		const auto &expected = context.expected;
		bool        ok       = true;
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
		    RunRemoveWithDependencies({.resolved_user = "alice", .positionals = {"3"}},
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

		auto [null_remove_result, null_remove_output] =
		    RunRemoveWithDependencies({.resolved_user = "alice", .positionals = {"3"}},
			                          {
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
		auto [result, output] = RunRemove(
		    context, {.resolved_user = "alice", .positionals = {"3"}, .assume_yes = true});
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
		auto [result, output] = RunRemove(
		    context, {.resolved_user = "alice", .positionals = {"3"}, .assume_yes = true});
		return Expect(result == 1, "stale removal returns 1") &&
		       Expect(output == "model changed\n", "stale removal forwards exact error") &&
		       Expect(context.remove_calls == 1, "stale removal called once");
	}

	auto LastModelSuccessPrintsDisabledMessage() -> bool {
		auto context                       = SuccessContext();
		context.remove_result.removed_last = true;
		auto [result, output]              = RunRemove(
		    context, {.resolved_user = "alice", .positionals = {"3"}, .assume_yes = true});
		return Expect(result == 0, "last-model removal returns 0") &&
		       Expect(output ==
		                  "Removed final face model; face verification disabled for this user\n",
		              "last-model removal preserves disabled message");
	}

	// The staged copy is dropped whenever the model may be gone, including uncertain commits,
	// and kept when the store reports that nothing changed.
	auto StagedCopiesPurgedWheneverRemovalMayHaveCommitted() -> bool {
		using howdy::native::UserModelStatus;
		bool ok = true;
		for (const auto &[status, expect_purge] : {
		         std::pair{UserModelStatus::kOk, true},
		         std::pair{UserModelStatus::kDurabilityUncertain, true},
		         std::pair{UserModelStatus::kCommitStateUncertain, true},
		         std::pair{UserModelStatus::kModelChanged, false},
		         std::pair{UserModelStatus::kWriteFailed, false},
		     }) {
			auto context                 = SuccessContext();
			context.remove_result.status = status;
			auto [result, output]        = RunRemove(
			    context, {.resolved_user = "alice", .positionals = {"3"}, .assume_yes = true});
			ok &= Expect(result == (status == UserModelStatus::kOk ? 0 : 1),
			             "only a clean removal returns 0");
			ok &= Expect(context.purge_calls == (expect_purge ? 1 : 0),
			             "staged copies are purged exactly when the removal may have committed");
			ok &= Expect(!expect_purge || context.purged_user == "alice",
			             "purge targets the removed user");
		}

		auto context          = SuccessContext();
		auto [result, output] = RunRemoveWithDependencies(
		    {.resolved_user = "alice", .positionals = {"3"}, .assume_yes = true},
		    {
		        .context                            = &context,
		        .list_user_model_entries            = ListCallback,
		        .remove_user_model_entry_if_matches = RemoveCallback,
		    });
		ok &= Expect(result == 0 && context.purge_calls == 0, "purge callback is optional");

		// With no models left, the command still retries a purge an earlier run could not finish.
		for (const auto status : {UserModelStatus::kNoModel, UserModelStatus::kNoModelDirectory}) {
			RemoveCliTestContext empty;
			empty.list_result.status          = status;
			auto [empty_result, empty_output] = RunRemove(
			    empty, {.resolved_user = "alice", .positionals = {"3"}, .assume_yes = true});
			ok &=
			    Expect(empty_result == 1 && empty.purge_calls == 1 && empty.purged_user == "alice",
				       "no stored models still purges staged copies");
		}

		// Repeating a remove for an ID that is already gone is the retry for a kept slot.
		auto repeated                           = SuccessContext();
		auto [repeated_result, repeated_output] = RunRemove(
		    repeated, {.resolved_user = "alice", .positionals = {"9"}, .assume_yes = true});
		ok &= Expect(
		    repeated_result == 1 && repeated.remove_calls == 0 && repeated.purge_calls == 1 &&
		        repeated.purge_trigger == howdy::native::auth_helper::StagedPurgeTrigger::kRetry,
		    "an unknown ID retries the purge without changing models");
		return ok;
	}

}  // namespace

auto main() -> int {
	bool ok = true;
	ok &= ListStatusesPreserveMessages();
	ok &= InvalidAndMissingIdsAbort();
	ok &= MissingUserOrPositionalAbortsBeforeStorage();
	ok &= RejectedConfirmationAborts();
	ok &= AcceptedConfirmationPassesCompleteExpectation();
	ok &= IncompleteDependenciesAbortWithoutCallbacks();
	ok &= YesFlagBypassesConfirmation();
	ok &= RemoveFailureForwardsError();
	ok &= LastModelSuccessPrintsDisabledMessage();
	ok &= StagedCopiesPurgedWheneverRemovalMayHaveCommitted();
	return ok ? 0 : 1;
}
