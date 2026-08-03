#include "prompt/prompt_coordinator_test_groups.hpp"
#include "prompt/prompt_coordinator_test_support.hpp"

namespace {
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
		std::atomic<int>  enter_count_seen{-1};
		std::atomic<bool> original_called{false};
	};

	struct ObservedWrapperState {
		int  original_calls = 0;
		int  observed_calls = 0;
		bool throw_observer = false;
	};

	auto batch_conversation(int num_msg, const struct pam_message **messages,
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

	void observe_secret_prompt(void *context) {
		auto &state = *static_cast<ObservedWrapperState *>(context);
		++state.observed_calls;
		if (state.throw_observer) {
			throw std::runtime_error("observer failure");
		}
	}

	auto blocking_password_conversation(int num_msg, const struct pam_message **messages,
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
			    return state->context->enter_presses.load() == 1;
		    })) {
			return PAM_CONV_ERR;
		}
		state->enter_count_seen = state->context->enter_presses.load();
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

	auto create_production_secret_prompt_conversation(void * /*context*/, pam_handle_t *pamh,
	                                                  howdy::pam::SecretPromptObserver observer)
	    -> std::unique_ptr<howdy::pam::SecretPromptConversation> {
		return std::make_unique<howdy::pam::ObservedPromptConversation>(pamh, observer);
	}

	auto strict_conversation(int num_msg, const struct pam_message **messages,
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

	auto test_watchdog_timeout_reaps_blocked_child() -> bool {
		const pid_t child_pid = spawn_blocked_child();
		if (!expect(child_pid > 0, "watchdog timeout child spawned")) {
			return false;
		}

		const int status = howdy::pam::compare_process::wait_until(
		    child_pid, std::chrono::steady_clock::now() + 40ms);
		return expect(status == timeout_wait_status(),
		              "watchdog timeout returns synthetic timeout status") &&
		       expect(child_reaped(child_pid), "watchdog timeout reaps blocked child");
	}

	auto test_watchdog_kills_sigterm_ignoring_child() -> bool {
		const pid_t child_pid = spawn_sigterm_ignoring_child();
		if (!expect(child_pid > 0, "SIGTERM-ignoring watchdog child spawned")) {
			return false;
		}

		const int status = howdy::pam::compare_process::wait_until(
		    child_pid, std::chrono::steady_clock::now() + 40ms);
		return expect(status == timeout_wait_status(),
		              "SIGTERM-ignoring child returns synthetic timeout status") &&
		       expect(child_reaped(child_pid), "SIGTERM-ignoring child is SIGKILLed and reaped");
	}

	auto test_watchdog_preserves_natural_exit_status() -> bool {
		const pid_t child_pid = spawn_child(17, 10ms);
		if (!expect(child_pid > 0, "natural watchdog child spawned")) {
			return false;
		}

		const int status = howdy::pam::compare_process::wait_until(
		    child_pid, std::chrono::steady_clock::now() + 1s);
		return expect(status == (17 << 8), "watchdog preserves natural exit wait status") &&
		       expect(child_reaped(child_pid), "watchdog reaps naturally exited child");
	}

	auto test_watchdog_timeout_keeps_password_fallback() -> bool {
		FakeContext context{
		    .token_delay  = 100ms,
		    .token_result = PAM_SUCCESS,
		};
		const pid_t child_pid = spawn_blocked_child();
		if (!expect(child_pid > 0, "watchdog fallback child spawned")) {
			return false;
		}
		context.next_child_pid        = child_pid;
		auto deps                     = dependencies(&context);
		deps.wait_for_compare_process = watchdog_wait_for_compare;

		PromptCoordinator coordinator(nullptr, Workaround::Input, true, false, deps, 40ms);
		const auto        result = coordinator.run(make_compare_request());
		return expect(result.decision == PromptCoordinatorDecision::kPasswordFallback,
		              "watchdog timeout keeps password fallback") &&
		       expect(result.compare_status == timeout_wait_status(),
		              "password fallback preserves watchdog timeout status") &&
		       expect(result.pam_status == PAM_SUCCESS,
		              "password fallback preserves PAM success") &&
		       expect(child_reaped(child_pid), "password fallback reaps watchdog child");
	}

	auto test_pam_success_reaps_before_watchdog() -> bool {
		FakeContext context;
		const pid_t child_pid = spawn_blocked_child();
		if (!expect(child_pid > 0, "PAM-before-watchdog child spawned")) {
			return false;
		}
		context.next_child_pid        = child_pid;
		auto deps                     = dependencies(&context);
		deps.wait_for_compare_process = watchdog_wait_for_compare;

		PromptCoordinator coordinator(nullptr, Workaround::Input, true, false, deps, 1s);
		const auto        result = coordinator.run(make_compare_request());
		return expect(result.decision == PromptCoordinatorDecision::kPamResult,
		              "PAM success wins before watchdog") &&
		       expect(context.terminate_calls == 1, "PAM success terminates compare child") &&
		       expect(context.last_wait_status != timeout_wait_status(),
		              "PAM success does not use watchdog timeout status") &&
		       expect(child_reaped(child_pid), "PAM success reaps compare child");
	}

	auto test_invalid_hard_timeout_fails_closed() -> bool {
		bool ok = true;
		for (const auto timeout : {std::chrono::milliseconds::zero(), -1ms}) {
			FakeContext       context;
			PromptCoordinator coordinator(nullptr, Workaround::Input, true, false,
			                              dependencies(&context), timeout);
			const auto        result = coordinator.run(make_compare_request());
			ok &= expect(!coordinator.valid(), "nonpositive hard timeout is invalid");
			ok &= expect(result.decision == PromptCoordinatorDecision::kInvalidDependencies,
			             "nonpositive hard timeout fails closed");
			ok &= expect(callback_counts(context) == CallbackCounts{},
			             "nonpositive hard timeout starts no callbacks");
		}
		return ok;
	}

	auto test_compare_wins_without_password_prompt() -> bool {
		FakeContext context;
		const pid_t child_pid = spawn_child(EXIT_SUCCESS);
		if (!expect(child_pid > 0, "compare-winner child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(nullptr, Workaround::Off, false, false,
		                              dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.run(make_compare_request());
		const bool        reaped = child_reaped(child_pid);
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

	auto test_pam_wins() -> bool {
		FakeContext context;
		context.run_thread    = std::this_thread::get_id();
		const pid_t child_pid = spawn_child(EXIT_SUCCESS, std::chrono::seconds(2));
		if (!expect(child_pid > 0, "PAM-winner child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(nullptr, Workaround::Input, true, false,
		                              dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.run(make_compare_request());
		const bool        reaped = child_reaped(child_pid);
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

	auto test_password_call_returned_before_enter_emission_suppresses_enter() -> bool {
		FakeContext       context;
		PromptCoordinator coordinator(nullptr, Workaround::Input, true, false,
		                              dependencies(&context), std::chrono::seconds(5));
		howdy::pam::PromptCoordinatorTestAccess::prepare_claimed_enter(
		    coordinator, std::make_unique<FakeEnterDevice>(&context));
		howdy::pam::PromptCoordinatorTestAccess::publish_password_call_returned(coordinator);
		howdy::pam::PromptCoordinatorTestAccess::send_enter_for_prompt_generations(coordinator);

		return expect(context.enter_presses == 0,
		              "password_call_returned suppresses claimed Enter before emission") &&
		       expect(!context.enter_emission_finished,
		              "suppressed Enter never enters device implementation");
	}

	auto test_enter_emission_started_before_password_return_allows_one_attempt() -> bool {
		FakeContext context{
		    .block_token_until_release = true,
		    .block_enter_after_emit    = true,
		};
		const pid_t child_pid = spawn_child(EXIT_SUCCESS);
		if (!expect(child_pid > 0, "emission-before-return child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(nullptr, Workaround::Input, true, false,
		                              dependencies(&context), std::chrono::seconds(5));
		context.coordinator_for_enter = &coordinator;
		howdy::pam::PromptCoordinatorResult result;
		std::thread                         run_thread([&] -> void {
			result = coordinator.run(make_compare_request());
		});

		bool ok = expect(wait_for_enter_ready(context, 1s),
		                 "in-flight Enter action begins before password returns");
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
			             "password call completes during Enter action");
		}
		ok &= expect(howdy::pam::PromptCoordinatorTestAccess::wait_for_password_call_returned(
		                 coordinator, 1s),
		             "password_call_returned does not wait for blocked Enter device");
		ok &= expect(!context.enter_emission_finished,
		             "blocked Enter emission has started but not finished");
		release_enter(context);
		run_thread.join();

		return ok &&
		       expect(context.enter_started_before_password_return,
		              "enter_emission_started before password_call_returned") &&
		       expect(context.enter_presses == 1, "in-flight Enter action emits once") &&
		       expect(context.enter_emission_finished, "Enter emission finishes after release") &&
		       expect(result.decision == PromptCoordinatorDecision::kHowdyResult,
		              "emission-before-return preserves compare winner") &&
		       expect(child_reaped(child_pid), "emission-before-return child is reaped");
	}

	auto test_prompt_generation_rollover_retries_canceled_claim() -> bool {
		FakeContext       context;
		PromptCoordinator coordinator(nullptr, Workaround::Input, true, false,
		                              dependencies(&context), 5s);
		howdy::pam::PromptCoordinatorTestAccess::prepare_claimed_enter(
		    coordinator, std::make_unique<FakeEnterDevice>(&context));
		howdy::pam::PromptCoordinatorTestAccess::close_prompt_generation(coordinator, 1);
		std::jthread worker([&coordinator] -> void {
			howdy::pam::PromptCoordinatorTestAccess::send_enter_for_prompt_generations(coordinator);
		});
		const auto   generation =
		    howdy::pam::PromptCoordinatorTestAccess::begin_prompt_generation(coordinator);
		const bool emitted = wait_for_enter_emission_finished(context, 1s);
		if (!emitted) {
			howdy::pam::PromptCoordinatorTestAccess::request_shutdown(coordinator);
		}
		worker.join();
		return expect(generation == 2, "later secret prompt receives new generation") &&
		       expect(emitted, "canceled claim retries on later prompt generation") &&
		       expect(context.enter_presses == 1, "generation rollover emits exactly one Enter");
	}

	auto test_password_return_between_generations_prevents_retry() -> bool {
		FakeContext       context;
		PromptCoordinator coordinator(nullptr, Workaround::Input, true, false,
		                              dependencies(&context), 5s);
		howdy::pam::PromptCoordinatorTestAccess::prepare_claimed_enter(
		    coordinator, std::make_unique<FakeEnterDevice>(&context));
		howdy::pam::PromptCoordinatorTestAccess::close_prompt_generation(coordinator, 1);
		howdy::pam::PromptCoordinatorTestAccess::publish_password_call_returned(coordinator);
		const auto generation =
		    howdy::pam::PromptCoordinatorTestAccess::begin_prompt_generation(coordinator);
		howdy::pam::PromptCoordinatorTestAccess::send_enter_for_prompt_generations(coordinator);
		return expect(generation == 0, "password return rejects later prompt generation") &&
		       expect(context.enter_presses == 0,
		              "password return between generations suppresses Enter retry");
	}

	auto test_shutdown_between_generations_prevents_retry() -> bool {
		FakeContext       context;
		PromptCoordinator coordinator(nullptr, Workaround::Input, true, false,
		                              dependencies(&context), 5s);
		howdy::pam::PromptCoordinatorTestAccess::prepare_claimed_enter(
		    coordinator, std::make_unique<FakeEnterDevice>(&context));
		howdy::pam::PromptCoordinatorTestAccess::close_prompt_generation(coordinator, 1);
		howdy::pam::PromptCoordinatorTestAccess::request_shutdown(coordinator);
		const auto generation =
		    howdy::pam::PromptCoordinatorTestAccess::begin_prompt_generation(coordinator);
		howdy::pam::PromptCoordinatorTestAccess::send_enter_for_prompt_generations(coordinator);
		return expect(generation == 0, "shutdown rejects later prompt generation") &&
		       expect(context.enter_presses == 0,
		              "shutdown between generations suppresses Enter retry");
	}

	auto test_generation_close_after_emission_starts_does_not_retry() -> bool {
		FakeContext       context{.block_enter_after_emit = true};
		PromptCoordinator coordinator(nullptr, Workaround::Input, true, false,
		                              dependencies(&context), 5s);
		howdy::pam::PromptCoordinatorTestAccess::prepare_claimed_enter(
		    coordinator, std::make_unique<FakeEnterDevice>(&context));
		std::jthread worker([&coordinator] -> void {
			howdy::pam::PromptCoordinatorTestAccess::send_enter_for_prompt_generations(coordinator);
		});
		bool         ok = expect(wait_for_enter_ready(context, 1s),
		                         "generation 1 Enter emission starts before close");
		howdy::pam::PromptCoordinatorTestAccess::close_prompt_generation(coordinator, 1);
		const auto generation =
		    howdy::pam::PromptCoordinatorTestAccess::begin_prompt_generation(coordinator);
		release_enter(context);
		worker.join();
		return ok && expect(generation == 2, "generation 2 begins after emission starts") &&
		       expect(context.enter_presses == 1,
		              "generation close after emission starts never sends second Enter");
	}

	auto test_multiple_generation_rollovers_emit_once() -> bool {
		FakeContext       context;
		PromptCoordinator coordinator(nullptr, Workaround::Input, true, false,
		                              dependencies(&context), 5s);
		howdy::pam::PromptCoordinatorTestAccess::prepare_claimed_enter(
		    coordinator, std::make_unique<FakeEnterDevice>(&context));
		howdy::pam::PromptCoordinatorTestAccess::close_prompt_generation(coordinator, 1);
		for (howdy::pam::SecretPromptGeneration expected = 2; expected < 5; ++expected) {
			const auto generation =
			    howdy::pam::PromptCoordinatorTestAccess::begin_prompt_generation(coordinator);
			if (!expect(generation == expected, "rollover generation advances monotonically")) {
				return false;
			}
			howdy::pam::PromptCoordinatorTestAccess::close_prompt_generation(coordinator,
			                                                                 generation);
		}
		std::jthread worker([&coordinator] -> void {
			howdy::pam::PromptCoordinatorTestAccess::send_enter_for_prompt_generations(coordinator);
		});
		const auto   final_generation =
		    howdy::pam::PromptCoordinatorTestAccess::begin_prompt_generation(coordinator);
		const bool emitted = wait_for_enter_emission_finished(context, 1s);
		if (!emitted) {
			howdy::pam::PromptCoordinatorTestAccess::request_shutdown(coordinator);
		}
		worker.join();
		return expect(final_generation == 5, "final rollover generation becomes active") &&
		       expect(emitted, "worker wakes after multiple generation rollovers") &&
		       expect(context.enter_presses == 1,
		              "multiple generation rollovers emit exactly one Enter");
	}

	auto test_application_conversation_stays_serial_on_caller_thread() -> bool {
		StrictConversationState conversation_state{
		    .caller_thread = std::this_thread::get_id(),
		};
		const struct pam_conv conversation{
		    .conv        = strict_conversation,
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
		const pid_t child_pid = spawn_blocked_child();
		if (!expect(child_pid > 0, "thread-affinity child spawned")) {
			pam_end(pamh, PAM_SYSTEM_ERR);
			return false;
		}
		context.next_child_pid = child_pid;
		PromptCoordinator coordinator(pamh, Workaround::Input, true, false, dependencies(&context),
		                              std::chrono::seconds(5));
		const auto        result = coordinator.run(make_compare_request());
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
		       expect(child_reaped(child_pid), "thread-affinity child is reaped");
	}

	auto test_best_effort_enter_follows_production_secret_prompt_observation() -> bool {
		FakeContext context{
		    .use_real_auth_token       = true,
		    .block_before_conversation = true,
		    .release_token_on_enter    = true,
		};
		PromptEntryState      state{.context = &context};
		const struct pam_conv conversation{
		    .conv        = blocking_password_conversation,
		    .appdata_ptr = &state,
		};
		pam_handle_t *pamh = nullptr;
		if (!expect(pam_start("howdy-observed-prompt-test", "alice", &conversation, &pamh) ==
		                PAM_SUCCESS,
		            "observed prompt test starts PAM transaction")) {
			return false;
		}

		const pid_t child_pid = spawn_child(EXIT_SUCCESS);
		if (!expect(child_pid > 0, "observed prompt child spawned")) {
			pam_end(pamh, PAM_SYSTEM_ERR);
			return false;
		}
		context.next_child_pid                 = child_pid;
		auto deps                              = dependencies(&context);
		deps.create_secret_prompt_conversation = create_production_secret_prompt_conversation;

		howdy::pam::PromptCoordinatorResult result;
		std::thread                         run_thread([&] -> void {
			context.run_thread = std::this_thread::get_id();
			PromptCoordinator coordinator(pamh, Workaround::Input, true, false, deps, 5s);
			result = coordinator.run(make_compare_request());
		});

		bool ok = true;
		{
			std::unique_lock<std::mutex> lock(context.token_mutex);
			ok &= expect(context.token_condition.wait_for(lock, 1s,
			                                              [&context] -> bool {
				                                              return context.before_conversation;
			                                              }),
			             "password acquisition pauses before wrapper receives secret prompt");
			ok &= expect(context.enter_presses == 0,
			             "face success sends no Enter before wrapper receives ECHO_OFF");
			context.release_conversation = true;
		}
		context.token_condition.notify_all();
		run_thread.join();
		pam_end(pamh, result.pam_status);

		return ok &&
		       expect(state.original_called, "original secret conversation receives prompt") &&
		       expect(state.enter_count_seen == 1,
		              "original callback may observe best-effort Enter already attempted") &&
		       expect(context.enter_presses == 1, "secret prompt permits exactly one Enter") &&
		       expect(result.decision == PromptCoordinatorDecision::kHowdyResult,
		              "immediate face success remains Howdy result") &&
		       expect(child_reaped(child_pid), "observed prompt child reaped");
	}

	auto test_password_call_returned_before_echo_off_sends_no_enter() -> bool {
		FakeContext context{
		    .use_real_auth_token           = true,
		    .block_before_conversation     = true,
		    .complete_without_conversation = true,
		};
		ObservedWrapperState  state;
		const struct pam_conv original{
		    .conv        = batch_conversation,
		    .appdata_ptr = &state,
		};
		pam_handle_t *pamh = nullptr;
		if (!expect(pam_start("howdy-no-secret-prompt-test", "alice", &original, &pamh) ==
		                PAM_SUCCESS,
		            "no-secret-prompt test starts PAM transaction")) {
			return false;
		}
		const pid_t child_pid = spawn_child(EXIT_SUCCESS);
		if (!expect(child_pid > 0, "no-secret-prompt child spawned")) {
			pam_end(pamh, PAM_SYSTEM_ERR);
			return false;
		}
		context.next_child_pid                 = child_pid;
		auto deps                              = dependencies(&context);
		deps.create_secret_prompt_conversation = create_production_secret_prompt_conversation;
		PromptCoordinator coordinator(pamh, Workaround::Input, true, false, deps, 5s);
		howdy::pam::PromptCoordinatorResult result;
		std::thread                         run_thread([&] -> void {
			result = coordinator.run(make_compare_request());
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
		ok &= expect(
		    howdy::pam::PromptCoordinatorTestAccess::wait_for_compare_success(coordinator, 1s),
		    "no-secret-prompt face success publishes before password_call_returned");
		{
			std::scoped_lock lock(context.token_mutex);
			context.release_conversation = true;
		}
		context.token_condition.notify_all();
		run_thread.join();
		pam_end(pamh, result.pam_status);

		return ok &&
		       expect(context.enter_presses == 0,
		              "password_call_returned before ECHO_OFF sends no Enter") &&
		       expect(state.original_calls == 0,
		              "password_call_returned before ECHO_OFF skips application conversation") &&
		       expect(result.decision == PromptCoordinatorDecision::kHowdyResult,
		              "face success remains winner before prompt-free password_call_returned") &&
		       expect(child_reaped(child_pid), "no-secret-prompt child reaped");
	}

	auto test_production_prompt_wrapper_batches_and_fails_closed() -> bool {
		ObservedWrapperState  state;
		const struct pam_conv original{
		    .conv        = batch_conversation,
		    .appdata_ptr = &state,
		};
		pam_handle_t *pamh = nullptr;
		if (!expect(pam_start("howdy-observed-wrapper-test", "alice", &original, &pamh) ==
		                PAM_SUCCESS,
		            "observed wrapper test starts PAM transaction")) {
			return false;
		}

		auto begin = [](void *context) -> howdy::pam::SecretPromptGeneration {
			observe_secret_prompt(context);
			return 1;
		};
		auto end = [](void *, howdy::pam::SecretPromptGeneration) -> void {};
		howdy::pam::ObservedPromptConversation wrapper(
		    pamh, {.context = &state, .begin = begin, .end = end});
		bool        ok = expect(wrapper.available(), "production observed wrapper is available") &&
		                 expect(wrapper.install() == PAM_SUCCESS,
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
		ok &= expect(wrapper.restore_original() ==
		                 howdy::pam::ConversationRestoreResult::kOriginalRestored,
		             "observed wrapper restores original conversation explicitly");
		pam_end(pamh, PAM_SUCCESS);
		return ok;
	}

	auto test_reaped_child_is_never_signalled_by_caller() -> bool {
		FakeContext context{
		    .hold_reaped_until_cancel = true,
		    .token_waits_for_reap     = true,
		};
		const pid_t child_pid = spawn_child(EXIT_SUCCESS);
		if (!expect(child_pid > 0, "reap/cancel race child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;
		PromptCoordinator coordinator(nullptr, Workaround::Input, true, false,
		                              dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.run(make_compare_request());
		return expect(result.decision == PromptCoordinatorDecision::kPamResult,
		              "password_call_returned wins while reaped wait is unpublished") &&
		       expect(context.wait_calls == 1, "reap/cancel race has one wait owner") &&
		       expect(context.terminate_calls == 0,
		              "reap/cancel race never signals already-reaped PID") &&
		       expect(child_reaped(child_pid), "reap/cancel race reaps child exactly once");
	}

	auto test_compare_wait_exception_reaps_child() -> bool {
		FakeContext context{.throw_compare_wait = true};
		const pid_t child_pid = spawn_blocked_child();
		if (!expect(child_pid > 0, "wait-exception child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;
		PromptCoordinator coordinator(nullptr, Workaround::Off, false, false,
		                              dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.run(make_compare_request());
		return expect(result.decision == PromptCoordinatorDecision::kHowdyResult,
		              "wait exception returns failed compare result") &&
		       expect(result.compare_status == (static_cast<int>(CompareExit::kAbort) << 8),
		              "wait exception maps to compare abort") &&
		       expect(child_reaped(child_pid), "wait exception emergency cleanup reaps child");
	}
}  // namespace

auto main() -> int {
	bool ok = true;
	ok &= test_watchdog_timeout_reaps_blocked_child();
	ok &= test_watchdog_kills_sigterm_ignoring_child();
	ok &= test_watchdog_preserves_natural_exit_status();
	ok &= test_watchdog_timeout_keeps_password_fallback();
	ok &= test_pam_success_reaps_before_watchdog();
	ok &= test_invalid_hard_timeout_fails_closed();
	ok &= test_compare_wins_without_password_prompt();
	ok &= test_pam_wins();
	ok &= test_password_call_returned_before_enter_emission_suppresses_enter();
	ok &= test_enter_emission_started_before_password_return_allows_one_attempt();
	ok &= test_prompt_generation_rollover_retries_canceled_claim();
	ok &= test_password_return_between_generations_prevents_retry();
	ok &= test_shutdown_between_generations_prevents_retry();
	ok &= test_generation_close_after_emission_starts_does_not_retry();
	ok &= test_multiple_generation_rollovers_emit_once();
	ok &= test_application_conversation_stays_serial_on_caller_thread();
	ok &= test_best_effort_enter_follows_production_secret_prompt_observation();
	ok &= test_password_call_returned_before_echo_off_sends_no_enter();
	ok &= test_production_prompt_wrapper_batches_and_fails_closed();
	ok &= test_reaped_child_is_never_signalled_by_caller();
	ok &= test_compare_wait_exception_reaps_child();
	ok &= run_prompt_mode_tests();
	ok &= run_prompt_adapter_tests();
	return ok ? 0 : 1;
}
