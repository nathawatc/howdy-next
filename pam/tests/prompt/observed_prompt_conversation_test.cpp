#include "prompt/observed_prompt_conversation.hpp"
#include "test_support.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include <security/pam_appl.h>

namespace howdy::pam {
	class ObservedPromptConversationTestAccess {
	public:
		using GetPamItemFn = int (*)(void *, pam_handle_t *, int, const void **);
		using SetPamItemFn = int (*)(void *, pam_handle_t *, int, const void *);

		struct InjectedOperations {
			void        *context;
			GetPamItemFn get_item;
			SetPamItemFn set_item;
		};

		static auto create(pam_handle_t *pamh, SecretPromptObserver observer,
		                   InjectedOperations injected_operations)
		    -> std::unique_ptr<ObservedPromptConversation> {
			auto operations     = ObservedPromptConversation::production_operations();
			operations.context  = injected_operations.context;
			operations.get_item = injected_operations.get_item;
			operations.set_item = injected_operations.set_item;
			return std::unique_ptr<ObservedPromptConversation>(
			    new ObservedPromptConversation(pamh, observer, operations));
		}

		static auto override_conversation(const ObservedPromptConversation &conversation)
		    -> struct pam_conv {
			return conversation.override_conv_;
		}
	};

}  // namespace howdy::pam

namespace {

	using howdy::pam::ConversationRestoreResult;
	using howdy::pam::ObservedPromptConversation;
	using howdy::pam::ObservedPromptConversationTestAccess;
	using howdy::test::expect;

	enum class ResponseMode : unsigned char {
		kNull,
		kAllocated,
		kPartial,
		kThrowAfterAllocation,
	};

	struct DispatchState {
		ResponseMode mode   = ResponseMode::kNull;
		int          result = PAM_CONV_ERR;
		int          calls  = 0;
	};

	struct OperationState {
		struct pam_conv    original{};
		std::array<int, 3> set_results{{PAM_SUCCESS, PAM_SUCCESS, PAM_SUCCESS}};
		int                get_calls = 0;
		int                set_calls = 0;
		struct pam_conv    last_set{};
	};

	struct ObserverState {
		int begin_calls = 0;
		int end_calls   = 0;
	};

	auto delegated_conversation(int num_msg, const struct pam_message **messages,
	                            struct pam_response **response, void *appdata_ptr) -> int {
		if (response != nullptr) {
			*response = nullptr;
		}
		auto *state = static_cast<DispatchState *>(appdata_ptr);
		if (state == nullptr || response == nullptr || num_msg <= 0 || messages == nullptr) {
			return PAM_CONV_ERR;
		}
		++state->calls;
		if (state->mode == ResponseMode::kNull) {
			return state->result;
		}

		auto *responses = static_cast<struct pam_response *>(
		    calloc(static_cast<std::size_t>(num_msg), sizeof(struct pam_response)));
		if (responses == nullptr) {
			return PAM_BUF_ERR;
		}
		if (state->mode == ResponseMode::kPartial) {
			responses[0].resp = strdup("partial-secret");
		} else {
			for (int index = 0; index < num_msg; ++index) {
				responses[index].resp = strdup("secret");
			}
		}
		*response = responses;
		if (state->mode == ResponseMode::kThrowAfterAllocation) {
			throw std::runtime_error("delegated conversation failure");
		}
		return state->result;
	}

	auto begin_prompt(void *context) -> howdy::pam::SecretPromptGeneration {
		auto &observer = *static_cast<ObserverState *>(context);
		return static_cast<howdy::pam::SecretPromptGeneration>(++observer.begin_calls);
	}

	void end_prompt(void *context, howdy::pam::SecretPromptGeneration /*generation*/) {
		++static_cast<ObserverState *>(context)->end_calls;
	}

	auto injected_get_item(void        *context, pam_handle_t        */*pamh*/, int /*item_type*/,
	                       const void **item) -> int {
		auto &state = *static_cast<OperationState *>(context);
		++state.get_calls;
		if (item == nullptr) {
			return PAM_SYSTEM_ERR;
		}
		*item = &state.original;
		return PAM_SUCCESS;
	}

	auto injected_set_item(void       *context, pam_handle_t       */*pamh*/, int /*item_type*/,
	                       const void *item) -> int {
		auto &state = *static_cast<OperationState *>(context);

		if (item != nullptr) {
			state.last_set = *static_cast<const struct pam_conv *>(item);
		}
		const auto index = static_cast<std::size_t>(state.set_calls++);
		return index < state.set_results.size() ? state.set_results[index] : PAM_SYSTEM_ERR;
	}

	auto make_dispatch_wrapper(OperationState *operations, DispatchState *dispatch,
	                           ObserverState *observer)
	    -> std::unique_ptr<ObservedPromptConversation> {
		operations->original = {
		    .conv        = delegated_conversation,
		    .appdata_ptr = dispatch,
		};
		return ObservedPromptConversationTestAccess::create(
		    reinterpret_cast<pam_handle_t *>(0x1),
		    {.context = observer, .begin = begin_prompt, .end = end_prompt},
		    {.context = operations, .get_item = injected_get_item, .set_item = injected_set_item});
	}

	auto make_messages(std::array<struct pam_message, 3>         *messages,
	                   std::array<const struct pam_message *, 3> *message_ptrs) -> void {
		*messages     = {{
		    {.msg_style = PAM_PROMPT_ECHO_ON, .msg = "User: "},
		    {.msg_style = PAM_PROMPT_ECHO_OFF, .msg = "Password: "},
		    {.msg_style = PAM_PROMPT_ECHO_OFF, .msg = "Password confirmation: "},
		}};
		*message_ptrs = {{messages->data(), messages->data() + 1, messages->data() + 2}};
	}

	auto test_response_cleanup_contract() -> bool {
		bool ok = true;

		{
			OperationState operations;
			DispatchState  dispatch{.mode = ResponseMode::kAllocated, .result = PAM_SUCCESS};
			ObserverState  observer;
			auto           wrapper = make_dispatch_wrapper(&operations, &dispatch, &observer);
			std::array<struct pam_message, 3>         messages{};
			std::array<const struct pam_message *, 3> message_ptrs{};
			make_messages(&messages, &message_ptrs);
			struct pam_response *responses = nullptr;
			const auto           conversation =
			    ObservedPromptConversationTestAccess::override_conversation(*wrapper);

			ok &= expect(conversation.conv(3, message_ptrs.data(), &responses,
			                               conversation.appdata_ptr) == PAM_SUCCESS,
			             "observed success preserves delegated response ownership");
			ok &= expect(responses != nullptr && std::string_view(responses[0].resp) == "secret",
			             "observed success returns valid response");
			ok &= expect(observer.begin_calls == 1 && observer.end_calls == 1,
			             "multiple ECHO_OFF messages form one closed prompt generation");
			if (responses != nullptr) {
				for (int index = 0; index < 3; ++index) {
					std::free(responses[index].resp);
				}
				std::free(responses);
			}
		}

		for (const auto mode : {ResponseMode::kNull, ResponseMode::kAllocated,
		                        ResponseMode::kPartial, ResponseMode::kThrowAfterAllocation}) {
			OperationState operations;
			DispatchState  dispatch{.mode = mode, .result = PAM_CONV_ERR};
			ObserverState  observer;
			auto           wrapper = make_dispatch_wrapper(&operations, &dispatch, &observer);
			std::array<struct pam_message, 3>         messages{};
			std::array<const struct pam_message *, 3> message_ptrs{};
			make_messages(&messages, &message_ptrs);
			auto      *responses = reinterpret_cast<struct pam_response *>(0x1);
			const auto conversation =
			    ObservedPromptConversationTestAccess::override_conversation(*wrapper);
			const int result =
			    conversation.conv(3, message_ptrs.data(), &responses, conversation.appdata_ptr);

			ok &= expect(result == PAM_CONV_ERR,
			             "observed delegated failure preserves PAM error result");
			ok &= expect(responses == nullptr,
			             "observed delegated failure always clears response ownership");
			ok &= expect(observer.begin_calls == 1 && observer.end_calls == 1,
			             "delegated failure or exception closes active prompt generation");
		}

		{
			OperationState operations;
			DispatchState  dispatch{.mode = ResponseMode::kAllocated, .result = PAM_CONV_ERR};
			ObserverState  observer;
			auto           wrapper = make_dispatch_wrapper(&operations, &dispatch, &observer);
			std::array<struct pam_message, 3>         messages{};
			std::array<const struct pam_message *, 3> message_ptrs{};
			make_messages(&messages, &message_ptrs);
			const auto conversation =
			    ObservedPromptConversationTestAccess::override_conversation(*wrapper);

			ok &= expect(conversation.conv(3, message_ptrs.data(), nullptr,
			                               conversation.appdata_ptr) == PAM_CONV_ERR,
			             "observed dispatcher rejects null output pointer");
			message_ptrs[1] = nullptr;
			auto *responses = reinterpret_cast<struct pam_response *>(0x1);
			ok &= expect(conversation.conv(3, message_ptrs.data(), &responses,
			                               conversation.appdata_ptr) == PAM_CONV_ERR,
			             "observed dispatcher rejects invalid message batch");
			ok &= expect(responses == nullptr, "observed invalid message batch leaves output null");
		}

		return ok;
	}

	auto call_late(const struct pam_conv &conversation) -> bool {
		const struct pam_message  message{.msg_style = PAM_TEXT_INFO, .msg = "late"};
		const struct pam_message *message_ptr = &message;
		auto                     *response    = reinterpret_cast<struct pam_response *>(0x1);
		return conversation.conv(1, &message_ptr, &response, conversation.appdata_ptr) ==
		           PAM_CONV_ERR &&
		       response == nullptr;
	}

	auto test_restore_result(ConversationRestoreResult expected, std::array<int, 3> set_results,
	                         const char *label) -> bool {
		OperationState operations{.set_results = set_results};
		DispatchState  dispatch;
		ObserverState  observer;
		auto           wrapper = make_dispatch_wrapper(&operations, &dispatch, &observer);
		bool ok = expect(wrapper->available(), std::string(label) + ": wrapper is available") &&
		          expect(wrapper->install() == PAM_SUCCESS,
		                 std::string(label) + ": wrapper installs through injected PAM operation");
		const auto override_conversation =
		    ObservedPromptConversationTestAccess::override_conversation(*wrapper);
		const auto result = wrapper->restore_original();
		ok &= expect(result == expected, std::string(label) + ": returns explicit restore result");
		const auto restored_conversation = operations.last_set;
		const int  calls_before_destroy  = operations.set_calls;
		wrapper.reset();
		ok &= expect(operations.set_calls == calls_before_destroy,
		             std::string(label) + ": destructor performs no PAM operation");

		if (expected == ConversationRestoreResult::kOriginalRestored) {
			ok &= expect(restored_conversation.conv == delegated_conversation,
			             std::string(label) + ": original callback is restored");
		} else {
			ok &= expect(call_late(restored_conversation),
			             std::string(label) + ": installed fail-closed callback rejects late call");
		}
		if (expected == ConversationRestoreResult::kUnsafe) {
			ok &= expect(call_late(override_conversation),
			             std::string(label) + ": quarantined callback rejects late call");
			ok &= expect(observer.begin_calls == 0 && observer.end_calls == 0,
			             std::string(label) + ": destroyed observer context is never reached");
		}
		return ok;
	}

}  // namespace

auto main() -> int {
	bool ok = true;
	ok &= test_response_cleanup_contract();
	ok &= test_restore_result(ConversationRestoreResult::kOriginalRestored,
	                          {PAM_SUCCESS, PAM_SUCCESS, PAM_SUCCESS}, "original restoration");
	ok &=
	    test_restore_result(ConversationRestoreResult::kFailClosedInstalled,
	                        {PAM_SUCCESS, PAM_SYSTEM_ERR, PAM_SUCCESS}, "fail-closed restoration");
	ok &= test_restore_result(ConversationRestoreResult::kUnsafe,
	                          {PAM_SUCCESS, PAM_SYSTEM_ERR, PAM_SYSTEM_ERR}, "unsafe restoration");
	return ok ? 0 : 1;
}
