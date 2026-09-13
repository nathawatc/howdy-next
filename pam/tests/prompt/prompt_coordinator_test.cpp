#include "prompt/prompt_coordinator_fake.hpp"
#include "prompt/prompt_coordinator_test_access.hpp"
#include "prompt/prompt_coordinator_test_groups.hpp"
#include "support/process_test_support.hpp"

#include <cstring>

namespace {
	using namespace howdy::test::process;
	using namespace howdy::test::prompt_coordinator;

	struct StrictConversationState {
		std::thread::id   caller_thread;
		std::atomic<bool> active{false};
		std::atomic<bool> wrong_thread{false};
		std::atomic<bool> overlapping{false};
		std::atomic<int>  calls{0};
	};

	struct PromptEntryState {
		FakeContext      *context = nullptr;
		std::atomic<int>  submission_count_seen{-1};
		std::atomic<bool> original_called{false};
	};

	struct ObservedWrapperState {
		int  original_calls = 0;
		int  observed_calls = 0;
		bool throw_observer = false;
	};

	auto BatchConversation(int num_msg, const struct pam_message **messages,
	                       struct pam_response **response, void *appdata_ptr) -> int {
		if (response != nullptr) {
			*response = nullptr;
		}
		auto *state = static_cast<ObservedWrapperState *>(appdata_ptr);
		if (state == nullptr || response == nullptr || num_msg != 2 || messages == nullptr) {
			return PAM_CONV_ERR;
		}
		++state->original_calls;
		auto *responses = static_cast<struct pam_response *>(
		    calloc(static_cast<std::size_t>(num_msg), sizeof(struct pam_response)));
		if (responses == nullptr) {
			return PAM_BUF_ERR;
		}
		responses[0].resp = strdup("user");
		responses[1].resp = strdup("secret");
		if (responses[0].resp == nullptr || responses[1].resp == nullptr) {
			std::free(responses[0].resp);
			std::free(responses[1].resp);
			std::free(responses);
			return PAM_BUF_ERR;
		}
		*response = responses;
		return PAM_SUCCESS;
	}

	void ObserveSecretPrompt(void *context) {
		auto &state = *static_cast<ObservedWrapperState *>(context);
		++state.observed_calls;
		if (state.throw_observer) {
			throw std::runtime_error("observer failure");
		}
	}

	auto BlockingPasswordConversation(int num_msg, const struct pam_message **messages,
	                                  struct pam_response **response, void *appdata_ptr) -> int {
		if (response != nullptr) {
			*response = nullptr;
		}
		auto *state = static_cast<PromptEntryState *>(appdata_ptr);
		if (state == nullptr || state->context == nullptr || response == nullptr || num_msg != 1 ||
		    messages == nullptr || messages[0] == nullptr ||
		    messages[0]->msg_style != PAM_PROMPT_ECHO_OFF) {
			return PAM_CONV_ERR;
		}
		state->original_called = true;
		state->context->token_condition.notify_all();

		std::unique_lock<std::mutex> lock(state->context->token_mutex);
		if (!state->context->token_condition.wait_for(lock, 1s, [state] -> bool {
			    return state->context->prompt_submissions.load() == 1;
		    })) {
			return PAM_CONV_ERR;
		}
		state->submission_count_seen = state->context->prompt_submissions.load();
		auto *responses =
		    static_cast<struct pam_response *>(calloc(1, sizeof(struct pam_response)));
		if (responses == nullptr) {
			return PAM_BUF_ERR;
		}
		responses[0].resp = strdup("password");
		if (responses[0].resp == nullptr) {
			std::free(responses);
			return PAM_BUF_ERR;
		}
		*response = responses;
		return PAM_SUCCESS;
	}

	auto CreateProductionSecretPromptConversation(void * /*context*/, pam_handle_t *pamh,
	                                              howdy::pam::SecretPromptObserver observer)
	    -> std::unique_ptr<howdy::pam::SecretPromptConversation> {
		return std::make_unique<howdy::pam::ObservedPromptConversation>(pamh, observer);
	}

	auto StrictConversation(int num_msg, const struct pam_message **messages,
	                        struct pam_response **response, void *appdata_ptr) -> int {
		if (response != nullptr) {
			*response = nullptr;
		}
		auto *state = static_cast<StrictConversationState *>(appdata_ptr);
		if (state == nullptr || response == nullptr || num_msg <= 0 || messages == nullptr) {
			return PAM_CONV_ERR;
		}
		if (std::this_thread::get_id() != state->caller_thread) {
			state->wrong_thread = true;
			return PAM_CONV_ERR;
		}
		if (state->active.exchange(true)) {
			state->overlapping = true;
			return PAM_CONV_ERR;
		}
		++state->calls;

		auto *responses = static_cast<struct pam_response *>(
		    calloc(static_cast<std::size_t>(num_msg), sizeof(struct pam_response)));
		if (responses == nullptr) {
			state->active = false;
			return PAM_BUF_ERR;
		}
		for (int index = 0; index < num_msg; ++index) {
			if (messages[index] == nullptr || (messages[index]->msg_style != PAM_PROMPT_ECHO_OFF &&
			                                   messages[index]->msg_style != PAM_PROMPT_ECHO_ON)) {
				for (int allocated = 0; allocated < index; ++allocated) {
					std::free(responses[allocated].resp);
				}
				std::free(responses);
				state->active = false;
				return PAM_CONV_ERR;
			}
			responses[index].resp = strdup("password");
			if (responses[index].resp == nullptr) {
				for (int allocated = 0; allocated < index; ++allocated) {
					std::free(responses[allocated].resp);
				}
				std::free(responses);
				state->active = false;
				return PAM_BUF_ERR;
			}
		}
		*response     = responses;
		state->active = false;
		return PAM_SUCCESS;
	}

	auto TestInvalidHardTimeoutFailsClosed() -> bool {
		bool ok = true;
		for (const auto timeout : {std::chrono::milliseconds::zero(), -1ms}) {
			FakeContext       context;
			PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false,
			                              Dependencies(&context), timeout);
			const auto        result = coordinator.Run(MakeCompareRequest());
			ok &= expect(!coordinator.Valid(), "nonpositive hard timeout is invalid");
			ok &= expect(result.decision == PromptCoordinatorDecision::kInvalidDependencies,
			             "nonpositive hard timeout fails closed");
			ok &= expect(GetCallbackCounts(context) == CallbackCounts{},
			             "nonpositive hard timeout starts no callbacks");
		}
		return ok;
	}

	auto TestCompareWinsWithoutPasswordPrompt() -> bool {
		FakeContext context;
		const pid_t child_pid = SpawnChild(EXIT_SUCCESS);
		if (!expect(child_pid > 0, "compare-winner child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(nullptr, Workaround::kOff, false, false,
		                              Dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.Run(MakeCompareRequest());
		const bool        reaped = ChildReaped(child_pid);
		return expect(result.decision == PromptCoordinatorDecision::kHowdyResult,
		              "compare winner returns Howdy result") &&
		       expect(context.spawn_calls == 1 && context.spawned_pid == child_pid,
		              "compare winner spawns child once") &&
		       expect(result.compare_status == 0,
		              "compare winner preserves exact successful wait status") &&
		       expect(context.wait_calls == 1 && context.waited_pid == child_pid,
		              "compare winner waits once for child") &&
		       expect(context.auth_token_calls == 0,
		              "compare winner does not request disabled password") &&
		       expect(context.terminate_calls == 0, "compare winner does not terminate child") &&
		       expect(reaped, "compare winner reaps child");
	}

	auto TestPamWins() -> bool {
		FakeContext context;
		context.run_thread    = std::this_thread::get_id();
		const pid_t child_pid = SpawnChild(EXIT_SUCCESS, std::chrono::seconds(2));
		if (!expect(child_pid > 0, "PAM-winner child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false,
		                              Dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.Run(MakeCompareRequest());
		const bool        reaped = ChildReaped(child_pid);
		return expect(result.decision == PromptCoordinatorDecision::kPamResult,
		              "PAM winner returns PAM result") &&
		       expect(context.spawn_calls == 1 && context.spawned_pid == child_pid,
		              "PAM winner spawns child once") &&
		       expect(context.wait_calls == 1 && context.waited_pid == child_pid,
		              "PAM winner waits for spawned child") &&
		       expect(result.pam_status == PAM_SUCCESS, "PAM winner preserves PAM success") &&
		       expect(context.preflight_calls == 1, "PAM winner runs input preflight once") &&
		       expect(context.auth_token_calls == 1, "PAM winner requests token once") &&
		       expect(context.auth_token_thread == context.run_thread,
		              "PAM winner requests token on run caller thread") &&
		       expect(context.wait_thread != context.run_thread,
		              "PAM winner waits for compare on worker thread") &&

		       expect(context.terminate_calls == 1 && context.terminated_pid == child_pid,
		              "PAM winner terminates compare child once") &&
		       expect(reaped, "PAM winner reaps compare child");
	}

	auto TestPasswordCallReturnedBeforePromptSubmissionSuppressesSubmission() -> bool {
		FakeContext       context;
		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false,
		                              Dependencies(&context), std::chrono::seconds(5));
		howdy::pam::PromptCoordinatorTestAccess::PrepareClaimedSubmission(
		    coordinator, std::make_unique<FakePromptSubmitter>(&context));
		howdy::pam::PromptCoordinatorTestAccess::PublishPasswordCallReturned(coordinator);
		howdy::pam::PromptCoordinatorTestAccess::SubmitPromptForGenerations(coordinator);

		return expect(context.prompt_submissions == 0,
		              "password_call_returned suppresses claimed prompt submission before "
		              "submission attempt") &&
		       expect(!context.submission_finished,
		              "suppressed prompt submission never reaches prompt submitter");
	}

	auto TestPromptSubmissionStartedBeforePasswordReturnAllowsOneAttempt() -> bool {
		FakeContext context{
		    .block_token_until_release    = true,
		    .block_submission_after_start = true,
		};
		const pid_t child_pid = SpawnChild(EXIT_SUCCESS);
		if (!expect(child_pid > 0, "submission-before-return child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false,
		                              Dependencies(&context), std::chrono::seconds(5));
		context.coordinator_for_submission = &coordinator;
		howdy::pam::PromptCoordinatorResult result;
		std::thread                         run_thread([&] -> void {
			result = coordinator.Run(MakeCompareRequest());
		});

		bool ok = expect(WaitForSubmissionReady(context, 1s),
		                 "in-flight prompt submission begins before password returns");
		{
			std::scoped_lock lock(context.token_mutex);
			context.release_token = true;
		}
		context.token_condition.notify_all();
		{
			std::unique_lock<std::mutex> lock(context.token_mutex);
			ok &= expect(context.token_condition.wait_for(lock, 1s,
			                                              [&context] -> bool {
				                                              return context.token_returned.load();
			                                              }),
			             "password call completes during prompt submission");
		}
		ok &= expect(
		    howdy::pam::PromptCoordinatorTestAccess::WaitForPasswordCallReturned(coordinator, 1s),
		    "password_call_returned does not wait for blocked prompt submitter");
		ok &= expect(!context.submission_finished,
		             "blocked prompt submission has started but not finished");
		ReleaseSubmission(context);
		run_thread.join();

		return ok &&
		       expect(context.submission_started_before_password_return,
		              "prompt_submission_started before password_call_returned") &&
		       expect(context.prompt_submissions == 1,
		              "in-flight submission attempt occurs once") &&
		       expect(context.submission_finished, "prompt submission finishes after release") &&
		       expect(result.decision == PromptCoordinatorDecision::kHowdyResult,
		              "submission-before-return preserves compare winner") &&
		       expect(ChildReaped(child_pid), "submission-before-return child is reaped");
	}

	auto TestPromptGenerationRolloverRetriesCanceledClaim() -> bool {
		FakeContext       context;
		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false,
		                              Dependencies(&context), 5s);
		howdy::pam::PromptCoordinatorTestAccess::PrepareClaimedSubmission(
		    coordinator, std::make_unique<FakePromptSubmitter>(&context));
		howdy::pam::PromptCoordinatorTestAccess::ClosePromptGeneration(coordinator, 1);
		std::jthread worker([&coordinator] -> void {
			howdy::pam::PromptCoordinatorTestAccess::SubmitPromptForGenerations(coordinator);
		});
		const auto   generation =
		    howdy::pam::PromptCoordinatorTestAccess::BeginPromptGeneration(coordinator);
		const bool submission_completed = WaitForSubmissionFinished(context, 1s);
		if (!submission_completed) {
			howdy::pam::PromptCoordinatorTestAccess::RequestShutdown(coordinator);
		}
		worker.join();
		return expect(generation == 2, "later secret prompt receives new generation") &&
		       expect(submission_completed, "canceled claim retries on later prompt generation") &&
		       expect(context.prompt_submissions == 1,
		              "generation rollover makes exactly one submission attempt");
	}

	auto TestPasswordReturnBetweenGenerationsPreventsRetry() -> bool {
		FakeContext       context;
		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false,
		                              Dependencies(&context), 5s);
		howdy::pam::PromptCoordinatorTestAccess::PrepareClaimedSubmission(
		    coordinator, std::make_unique<FakePromptSubmitter>(&context));
		howdy::pam::PromptCoordinatorTestAccess::ClosePromptGeneration(coordinator, 1);
		howdy::pam::PromptCoordinatorTestAccess::PublishPasswordCallReturned(coordinator);
		const auto generation =
		    howdy::pam::PromptCoordinatorTestAccess::BeginPromptGeneration(coordinator);
		howdy::pam::PromptCoordinatorTestAccess::SubmitPromptForGenerations(coordinator);
		return expect(generation == 0, "password return rejects later prompt generation") &&
		       expect(context.prompt_submissions == 0,
		              "password return between generations suppresses prompt submission retry");
	}

	auto TestShutdownBetweenGenerationsPreventsRetry() -> bool {
		FakeContext       context;
		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false,
		                              Dependencies(&context), 5s);
		howdy::pam::PromptCoordinatorTestAccess::PrepareClaimedSubmission(
		    coordinator, std::make_unique<FakePromptSubmitter>(&context));
		howdy::pam::PromptCoordinatorTestAccess::ClosePromptGeneration(coordinator, 1);
		howdy::pam::PromptCoordinatorTestAccess::RequestShutdown(coordinator);
		const auto generation =
		    howdy::pam::PromptCoordinatorTestAccess::BeginPromptGeneration(coordinator);
		howdy::pam::PromptCoordinatorTestAccess::SubmitPromptForGenerations(coordinator);
		return expect(generation == 0, "shutdown rejects later prompt generation") &&
		       expect(context.prompt_submissions == 0,
		              "shutdown between generations suppresses prompt submission retry");
	}

	auto TestGenerationCloseAfterSubmissionStartsDoesNotRetry() -> bool {
		FakeContext       context{.block_submission_after_start = true};
		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false,
		                              Dependencies(&context), 5s);
		howdy::pam::PromptCoordinatorTestAccess::PrepareClaimedSubmission(
		    coordinator, std::make_unique<FakePromptSubmitter>(&context));
		std::jthread worker([&coordinator] -> void {
			howdy::pam::PromptCoordinatorTestAccess::SubmitPromptForGenerations(coordinator);
		});
		bool         ok = expect(WaitForSubmissionReady(context, 1s),
		                         "generation 1 prompt submission starts before close");
		howdy::pam::PromptCoordinatorTestAccess::ClosePromptGeneration(coordinator, 1);
		const auto generation =
		    howdy::pam::PromptCoordinatorTestAccess::BeginPromptGeneration(coordinator);
		ReleaseSubmission(context);
		worker.join();
		return ok && expect(generation == 2, "generation 2 begins after submission starts") &&
		       expect(context.prompt_submissions == 1, "generation close after submission starts "
		                                               "never makes second submission attempt");
	}

	auto TestMultipleGenerationRolloversSubmitOnce() -> bool {
		FakeContext       context;
		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false,
		                              Dependencies(&context), 5s);
		howdy::pam::PromptCoordinatorTestAccess::PrepareClaimedSubmission(
		    coordinator, std::make_unique<FakePromptSubmitter>(&context));
		howdy::pam::PromptCoordinatorTestAccess::ClosePromptGeneration(coordinator, 1);
		for (howdy::pam::SecretPromptGeneration expected = 2; expected < 5; ++expected) {
			const auto generation =
			    howdy::pam::PromptCoordinatorTestAccess::BeginPromptGeneration(coordinator);
			if (!expect(generation == expected, "rollover generation advances monotonically")) {
				return false;
			}
			howdy::pam::PromptCoordinatorTestAccess::ClosePromptGeneration(coordinator, generation);
		}
		std::jthread worker([&coordinator] -> void {
			howdy::pam::PromptCoordinatorTestAccess::SubmitPromptForGenerations(coordinator);
		});
		const auto   final_generation =
		    howdy::pam::PromptCoordinatorTestAccess::BeginPromptGeneration(coordinator);
		const bool submission_completed = WaitForSubmissionFinished(context, 1s);
		if (!submission_completed) {
			howdy::pam::PromptCoordinatorTestAccess::RequestShutdown(coordinator);
		}
		worker.join();
		return expect(final_generation == 5, "final rollover generation becomes active") &&
		       expect(submission_completed, "worker wakes after multiple generation rollovers") &&
		       expect(context.prompt_submissions == 1,
		              "multiple generation rollovers make exactly one submission attempt");
	}

	auto TestApplicationConversationStaysSerialOnCallerThread() -> bool {
		StrictConversationState conversation_state{
		    .caller_thread = std::this_thread::get_id(),
		};
		const struct pam_conv conversation{
		    .conv        = StrictConversation,
		    .appdata_ptr = &conversation_state,
		};
		pam_handle_t *pamh = nullptr;
		if (!expect(pam_start("howdy-thread-affinity-test", "alice", &conversation, &pamh) ==
		                PAM_SUCCESS,
		            "thread-affinity test starts PAM transaction")) {
			return false;
		}

		FakeContext context{.use_real_auth_token = true};
		context.run_thread    = std::this_thread::get_id();
		const pid_t child_pid = SpawnBlockedChild();
		if (!expect(child_pid > 0, "thread-affinity child spawned")) {
			pam_end(pamh, PAM_SYSTEM_ERR);
			return false;
		}
		context.next_child_pid = child_pid;
		PromptCoordinator coordinator(pamh, Workaround::kInput, true, false, Dependencies(&context),
		                              std::chrono::seconds(5));
		const auto        result = coordinator.Run(MakeCompareRequest());
		pam_end(pamh, result.pam_status);

		return expect(result.decision == PromptCoordinatorDecision::kPamResult,
		              "thread-affinity password wins") &&
		       expect(result.pam_status == PAM_SUCCESS,
		              "thread-affinity real PAM token request succeeds") &&
		       expect(conversation_state.calls == 1,
		              "thread-affinity invokes application conversation once") &&
		       expect(!conversation_state.wrong_thread,
		              "application conversation stays on run caller thread") &&
		       expect(!conversation_state.overlapping && !conversation_state.active,
		              "application conversation never overlaps or reenters") &&
		       expect(context.wait_thread != context.run_thread,
		              "application conversation remains separate from compare worker") &&
		       expect(ChildReaped(child_pid), "thread-affinity child is reaped");
	}

	auto TestBestEffortPromptSubmissionFollowsProductionSecretPromptObservation() -> bool {
		FakeContext context{
		    .use_real_auth_token         = true,
		    .block_before_conversation   = true,
		    .release_token_on_submission = true,
		};
		PromptEntryState      state{.context = &context};
		const struct pam_conv conversation{
		    .conv        = BlockingPasswordConversation,
		    .appdata_ptr = &state,
		};
		pam_handle_t *pamh = nullptr;
		if (!expect(pam_start("howdy-observed-prompt-test", "alice", &conversation, &pamh) ==
		                PAM_SUCCESS,
		            "observed prompt test starts PAM transaction")) {
			return false;
		}

		const pid_t child_pid = SpawnChild(EXIT_SUCCESS);
		if (!expect(child_pid > 0, "observed prompt child spawned")) {
			pam_end(pamh, PAM_SYSTEM_ERR);
			return false;
		}
		context.next_child_pid                 = child_pid;
		auto deps                              = Dependencies(&context);
		deps.create_secret_prompt_conversation = CreateProductionSecretPromptConversation;

		howdy::pam::PromptCoordinatorResult result;
		std::thread                         run_thread([&] -> void {
			context.run_thread = std::this_thread::get_id();
			PromptCoordinator coordinator(pamh, Workaround::kInput, true, false, deps, 5s);
			result = coordinator.Run(MakeCompareRequest());
		});

		bool ok = true;
		{
			std::unique_lock<std::mutex> lock(context.token_mutex);
			ok &= expect(context.token_condition.wait_for(lock, 1s,
			                                              [&context] -> bool {
				                                              return context.before_conversation;
			                                              }),
			             "password acquisition pauses before wrapper receives secret prompt");
			ok &=
			    expect(context.prompt_submissions == 0,
			           "face success makes no submission attempt before wrapper receives ECHO_OFF");
			context.release_conversation = true;
		}
		context.token_condition.notify_all();
		run_thread.join();
		pam_end(pamh, result.pam_status);

		return ok &&
		       expect(state.original_called, "original secret conversation receives prompt") &&
		       expect(
		           state.submission_count_seen == 1,
		           "original callback may observe best-effort submission attempt already made") &&
		       expect(context.prompt_submissions == 1,
		              "secret prompt permits exactly one submission attempt") &&
		       expect(result.decision == PromptCoordinatorDecision::kHowdyResult,
		              "immediate face success remains Howdy result") &&
		       expect(ChildReaped(child_pid), "observed prompt child reaped");
	}

	auto TestPasswordCallReturnedBeforeEchoOffSubmitsNoPrompt() -> bool {
		FakeContext context{
		    .use_real_auth_token           = true,
		    .block_before_conversation     = true,
		    .complete_without_conversation = true,
		};
		ObservedWrapperState  state;
		const struct pam_conv original{
		    .conv        = BatchConversation,
		    .appdata_ptr = &state,
		};
		pam_handle_t *pamh = nullptr;
		if (!expect(pam_start("howdy-no-secret-prompt-test", "alice", &original, &pamh) ==
		                PAM_SUCCESS,
		            "no-secret-prompt test starts PAM transaction")) {
			return false;
		}
		const pid_t child_pid = SpawnChild(EXIT_SUCCESS);
		if (!expect(child_pid > 0, "no-secret-prompt child spawned")) {
			pam_end(pamh, PAM_SYSTEM_ERR);
			return false;
		}
		context.next_child_pid                 = child_pid;
		auto deps                              = Dependencies(&context);
		deps.create_secret_prompt_conversation = CreateProductionSecretPromptConversation;
		PromptCoordinator coordinator(pamh, Workaround::kInput, true, false, deps, 5s);
		howdy::pam::PromptCoordinatorResult result;
		std::thread                         run_thread([&] -> void {
			result = coordinator.Run(MakeCompareRequest());
		});

		bool ok = true;
		{
			std::unique_lock<std::mutex> lock(context.token_mutex);
			ok &= expect(context.token_condition.wait_for(lock, 1s,
			                                              [&context] -> bool {
				                                              return context.before_conversation;
			                                              }),
			             "no-secret-prompt password request reaches pre-conversation barrier");
		}
		ok &=
		    expect(howdy::pam::PromptCoordinatorTestAccess::WaitForCompareSuccess(coordinator, 1s),
		           "no-secret-prompt face success publishes before password_call_returned");
		{
			std::scoped_lock lock(context.token_mutex);
			context.release_conversation = true;
		}
		context.token_condition.notify_all();
		run_thread.join();
		pam_end(pamh, result.pam_status);

		return ok &&
		       expect(context.prompt_submissions == 0,
		              "password_call_returned before ECHO_OFF makes no submission attempt") &&
		       expect(state.original_calls == 0,
		              "password_call_returned before ECHO_OFF skips application conversation") &&
		       expect(result.decision == PromptCoordinatorDecision::kHowdyResult,
		              "face success remains winner before prompt-free password_call_returned") &&
		       expect(ChildReaped(child_pid), "no-secret-prompt child reaped");
	}

	auto TestPromptSubmissionFailureIsHandled() -> bool {
		FakeContext       context{.fail_prompt_submission = true};
		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false,
		                              Dependencies(&context), std::chrono::seconds(5));
		howdy::pam::PromptCoordinatorTestAccess::PrepareClaimedSubmission(
		    coordinator, std::make_unique<FakePromptSubmitter>(&context));
		howdy::pam::PromptCoordinatorTestAccess::SubmitPromptForGenerations(coordinator);

		return expect(context.prompt_submissions == 1,
		              "prompt submission failure attempts submission once") &&
		       expect(
		           howdy::pam::PromptCoordinatorTestAccess::PromptSubmissionFinished(coordinator),
		           "prompt submission exception is contained and marks completion");
	}

	auto TestProductionPromptWrapperBatchesAndFailsClosed() -> bool {
		ObservedWrapperState  state;
		const struct pam_conv original{
		    .conv        = BatchConversation,
		    .appdata_ptr = &state,
		};
		pam_handle_t *pamh = nullptr;
		if (!expect(pam_start("howdy-observed-wrapper-test", "alice", &original, &pamh) ==
		                PAM_SUCCESS,
		            "observed wrapper test starts PAM transaction")) {
			return false;
		}

		auto begin = [](void *context) -> howdy::pam::SecretPromptGeneration {
			ObserveSecretPrompt(context);
			return 1;
		};
		auto end = [](void *, howdy::pam::SecretPromptGeneration) -> void {};
		howdy::pam::ObservedPromptConversation wrapper(
		    pamh, {.context = &state, .begin = begin, .end = end});
		bool        ok = expect(wrapper.Available(), "production observed wrapper is available") &&
		                 expect(wrapper.Install() == PAM_SUCCESS,
		                        "production observed wrapper installs on caller thread");
		const void *item = nullptr;
		ok &= expect(pam_get_item(pamh, PAM_CONV, &item) == PAM_SUCCESS && item != nullptr,
		             "observed wrapper test reads installed conversation");
		const auto *installed = static_cast<const struct pam_conv *>(item);
		const std::array<struct pam_message, 2>   messages{{
		    {.msg_style = PAM_PROMPT_ECHO_ON, .msg = "User: "},
		    {.msg_style = PAM_PROMPT_ECHO_OFF, .msg = "Password: "},
		}};
		std::array<const struct pam_message *, 2> message_ptrs{
		    {messages.data(), messages.data() + 1}};
		auto *responses = reinterpret_cast<struct pam_response *>(0x1);
		ok &= expect(installed->conv(2, message_ptrs.data(), &responses, installed->appdata_ptr) ==
		                 PAM_SUCCESS,
		             "observed wrapper delegates mixed prompt batch synchronously");
		ok &= expect(state.observed_calls == 1,
		             "observed wrapper publishes one secret observation per batch");
		ok &= expect(state.original_calls == 1,
		             "observed wrapper invokes original conversation once");
		ok &= expect(responses != nullptr && std::string_view(responses[0].resp) == "user" &&
		                 std::string_view(responses[1].resp) == "secret",
		             "observed wrapper preserves original response ownership and contents");
		if (responses != nullptr) {
			std::free(responses[0].resp);
			std::free(responses[1].resp);
			std::free(responses);
		}

		std::array<const struct pam_message *, 2> invalid_messages{{messages.data(), nullptr}};
		responses = reinterpret_cast<struct pam_response *>(0x1);
		ok &= expect(installed->conv(2, invalid_messages.data(), &responses,
		                             installed->appdata_ptr) == PAM_CONV_ERR,
		             "observed wrapper rejects invalid message batch");
		ok &= expect(responses == nullptr, "observed wrapper clears response before validation");

		state.throw_observer = true;
		responses            = reinterpret_cast<struct pam_response *>(0x1);
		ok &= expect(installed->conv(2, message_ptrs.data(), &responses, installed->appdata_ptr) ==
		                 PAM_CONV_ERR,
		             "observed wrapper catches observer exception before C ABI return");
		ok &= expect(responses == nullptr, "observer exception leaves no response");
		ok &= expect(wrapper.RestoreOriginal() ==
		                 howdy::pam::ConversationRestoreResult::kOriginalRestored,
		             "observed wrapper restores original conversation explicitly");
		pam_end(pamh, PAM_SUCCESS);
		return ok;
	}

	auto TestReapedChildIsNeverSignalledByCaller() -> bool {
		FakeContext context{
		    .hold_reaped_until_cancel = true,
		    .token_waits_for_reap     = true,
		};
		const pid_t child_pid = SpawnChild(EXIT_SUCCESS);
		if (!expect(child_pid > 0, "reap/cancel race child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;
		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false,
		                              Dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.Run(MakeCompareRequest());
		return expect(result.decision == PromptCoordinatorDecision::kPamResult,
		              "password_call_returned wins while reaped wait is unpublished") &&
		       expect(context.wait_calls == 1, "reap/cancel race has one wait owner") &&
		       expect(context.terminate_calls == 0,
		              "reap/cancel race never signals already-reaped PID") &&
		       expect(ChildReaped(child_pid), "reap/cancel race reaps child exactly once");
	}

	auto TestStalePromptGenerationCloseIsIgnored() -> bool {
		FakeContext       context;
		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false,
		                              Dependencies(&context), 5s);
		howdy::pam::PromptCoordinatorTestAccess::ClosePromptGeneration(coordinator, 0);
		howdy::pam::PromptCoordinatorTestAccess::PrepareClaimedSubmission(
		    coordinator, std::make_unique<FakePromptSubmitter>(&context));
		const auto generation =
		    howdy::pam::PromptCoordinatorTestAccess::BeginPromptGeneration(coordinator);
		howdy::pam::PromptCoordinatorTestAccess::ClosePromptGeneration(coordinator, generation - 1);
		std::jthread worker([&coordinator] -> void {
			howdy::pam::PromptCoordinatorTestAccess::SubmitPromptForGenerations(coordinator);
		});
		const bool   submission_completed = WaitForSubmissionFinished(context, 1s);
		if (!submission_completed) {
			howdy::pam::PromptCoordinatorTestAccess::RequestShutdown(coordinator);
		}
		worker.join();
		return expect(generation == 2, "stale generation test starts second generation") &&
		       expect(submission_completed, "stale generation close preserves active generation") &&
		       expect(context.prompt_submissions == 1,
		              "stale generation close permits one prompt submission");
	}

	auto TestCompareWaitExceptionReapsChild() -> bool {
		FakeContext context{.throw_compare_wait = true};
		const pid_t child_pid = SpawnBlockedChild();
		if (!expect(child_pid > 0, "wait-exception child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;
		PromptCoordinator coordinator(nullptr, Workaround::kOff, false, false,
		                              Dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.Run(MakeCompareRequest());
		return expect(result.decision == PromptCoordinatorDecision::kHowdyResult,
		              "wait exception returns failed compare result") &&
		       expect(result.compare_status == (static_cast<int>(CompareExit::kAbort) << 8),
		              "wait exception maps to compare abort") &&
		       expect(context.cleanup_calls == 1 && context.cleaned_pid == child_pid,
		              "wait exception invokes injected cleanup once") &&
		       expect(ChildReaped(child_pid), "wait exception emergency cleanup reaps child");
	}
}  // namespace

auto main() -> int {
	bool ok = true;
	ok &= TestInvalidHardTimeoutFailsClosed();
	ok &= TestCompareWinsWithoutPasswordPrompt();
	ok &= TestPamWins();
	ok &= TestPasswordCallReturnedBeforePromptSubmissionSuppressesSubmission();
	ok &= TestPromptSubmissionStartedBeforePasswordReturnAllowsOneAttempt();
	ok &= TestPromptGenerationRolloverRetriesCanceledClaim();
	ok &= TestPasswordReturnBetweenGenerationsPreventsRetry();
	ok &= TestShutdownBetweenGenerationsPreventsRetry();
	ok &= TestGenerationCloseAfterSubmissionStartsDoesNotRetry();
	ok &= TestMultipleGenerationRolloversSubmitOnce();
	ok &= TestApplicationConversationStaysSerialOnCallerThread();
	ok &= TestBestEffortPromptSubmissionFollowsProductionSecretPromptObservation();
	ok &= TestPasswordCallReturnedBeforeEchoOffSubmitsNoPrompt();
	ok &= TestPromptSubmissionFailureIsHandled();
	ok &= TestProductionPromptWrapperBatchesAndFailsClosed();
	ok &= TestReapedChildIsNeverSignalledByCaller();
	ok &= TestStalePromptGenerationCloseIsIgnored();
	ok &= TestCompareWaitExceptionReapsChild();
	ok &= RunPromptModeTests();
	ok &= RunPromptAdapterTests();
	return ok ? 0 : 1;
}
