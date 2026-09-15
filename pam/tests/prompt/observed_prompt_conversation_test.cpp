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

		static auto Create(pam_handle_t *pamh, SecretPromptObserver observer,
		                   InjectedOperations injected_operations)
		    -> std::unique_ptr<ObservedPromptConversation> {
			auto operations     = ObservedPromptConversation::ProductionOperations();
			operations.context  = injected_operations.context;
			operations.get_item = injected_operations.get_item;
			operations.set_item = injected_operations.set_item;
			return std::unique_ptr<ObservedPromptConversation>(
			    new ObservedPromptConversation(pamh, observer, operations));
		}

		static auto OverrideConversation(const ObservedPromptConversation &conversation)
		    -> struct pam_conv {
			return conversation.override_conv_;
		}
	};

}  // namespace howdy::pam

namespace {

	using howdy::pam::ConversationRestoreResult;
	using howdy::pam::ObservedPromptConversation;
	using howdy::pam::ObservedPromptConversationTestAccess;
	using howdy::pam::SecretPromptObserver;
	using howdy::test::Expect;

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
		int                get_calls        = 0;
		int                set_calls        = 0;
		int                get_result       = PAM_SUCCESS;
		bool               return_null_item = false;
		struct pam_conv    last_set{};
	};

	struct ObserverState {
		int  begin_calls = 0;
		int  end_calls   = 0;
		bool throw_end   = false;
	};

	auto DelegatedConversation(int num_msg, const struct pam_message **messages,
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

	auto BeginPrompt(void *context) -> howdy::pam::SecretPromptGeneration {
		auto &observer = *static_cast<ObserverState *>(context);
		return static_cast<howdy::pam::SecretPromptGeneration>(++observer.begin_calls);
	}

	void EndPrompt(void *context, howdy::pam::SecretPromptGeneration /*generation*/) {
		auto &observer = *static_cast<ObserverState *>(context);
		++observer.end_calls;
		if (observer.throw_end) {
			throw std::runtime_error("observer cleanup failure");
		}
	}

	auto InjectedGetItem(void        *context, pam_handle_t        */*pamh*/, int /*item_type*/,
	                     const void **item) -> int {
		auto &state = *static_cast<OperationState *>(context);
		++state.get_calls;
		if (item == nullptr) {
			return PAM_SYSTEM_ERR;
		}
		if (state.get_result != PAM_SUCCESS) {
			return state.get_result;
		}
		*item = state.return_null_item ? nullptr : &state.original;
		return PAM_SUCCESS;
	}

	auto InjectedSetItem(void       *context, pam_handle_t       */*pamh*/, int /*item_type*/,
	                     const void *item) -> int {
		auto &state = *static_cast<OperationState *>(context);

		if (item != nullptr) {
			state.last_set = *static_cast<const struct pam_conv *>(item);
		}
		const auto index = static_cast<std::size_t>(state.set_calls++);
		return index < state.set_results.size() ? state.set_results[index] : PAM_SYSTEM_ERR;
	}

	auto MakeDispatchWrapper(OperationState *operations, DispatchState *dispatch,
	                         ObserverState *observer)
	    -> std::unique_ptr<ObservedPromptConversation> {
		operations->original = {
		    .conv        = DelegatedConversation,
		    .appdata_ptr = dispatch,
		};
		return ObservedPromptConversationTestAccess::Create(
		    reinterpret_cast<pam_handle_t *>(0x1),
		    {.context = observer, .begin = BeginPrompt, .end = EndPrompt},
		    {.context = operations, .get_item = InjectedGetItem, .set_item = InjectedSetItem});
	}

	auto MakeMessages(std::array<struct pam_message, 3>         *messages,
	                  std::array<const struct pam_message *, 3> *message_ptrs) -> void {
		*messages     = {{
		    {.msg_style = PAM_PROMPT_ECHO_ON, .msg = "User: "},
		    {.msg_style = PAM_PROMPT_ECHO_OFF, .msg = "Password: "},
		    {.msg_style = PAM_PROMPT_ECHO_OFF, .msg = "Password confirmation: "},
		}};
		*message_ptrs = {{messages->data(), messages->data() + 1, messages->data() + 2}};
	}

	auto TestResponseCleanupContract() -> bool {
		bool ok = true;

		{
			OperationState operations;
			DispatchState  dispatch{.mode = ResponseMode::kAllocated, .result = PAM_SUCCESS};
			ObserverState  observer;
			auto           wrapper = MakeDispatchWrapper(&operations, &dispatch, &observer);
			std::array<struct pam_message, 3>         messages{};
			std::array<const struct pam_message *, 3> message_ptrs{};
			MakeMessages(&messages, &message_ptrs);
			struct pam_response *responses = nullptr;
			const auto           conversation =
			    ObservedPromptConversationTestAccess::OverrideConversation(*wrapper);

			ok &= Expect(conversation.conv(3, message_ptrs.data(), &responses,
			                               conversation.appdata_ptr) == PAM_SUCCESS,
			             "observed success preserves delegated response ownership");
			ok &= Expect(responses != nullptr && std::string_view(responses[0].resp) == "secret",
			             "observed success returns valid response");
			ok &= Expect(observer.begin_calls == 1 && observer.end_calls == 1,
			             "multiple ECHO_OFF messages form one closed prompt generation");
			if (responses != nullptr) {
				for (int index = 0; index < 3; ++index) {
					std::free(responses[index].resp);
				}
				std::free(responses);
			}
		}

		{
			OperationState operations;
			DispatchState  dispatch{.mode = ResponseMode::kNull, .result = PAM_SUCCESS};
			ObserverState  observer{.throw_end = true};
			auto           wrapper = MakeDispatchWrapper(&operations, &dispatch, &observer);
			std::array<struct pam_message, 3>         messages{};
			std::array<const struct pam_message *, 3> message_ptrs{};
			MakeMessages(&messages, &message_ptrs);
			auto      *responses = reinterpret_cast<struct pam_response *>(0x1);
			const auto conversation =
			    ObservedPromptConversationTestAccess::OverrideConversation(*wrapper);
			ok &= Expect(conversation.conv(3, message_ptrs.data(), &responses,
			                               conversation.appdata_ptr) == PAM_SUCCESS,
			             "observer end exception does not change delegated success");
			ok &= Expect(observer.begin_calls == 1 && observer.end_calls == 1,
			             "observer end exception still closes prompt generation");
			ok &= Expect(responses == nullptr, "observer end exception leaves response null");
		}

		{
			OperationState operations;
			DispatchState  dispatch{.mode = ResponseMode::kNull, .result = PAM_SUCCESS};
			ObserverState  observer;
			auto           wrapper = MakeDispatchWrapper(&operations, &dispatch, &observer);
			const struct pam_message  message{.msg_style = PAM_TEXT_INFO, .msg = "notice"};
			const struct pam_message *message_ptr = &message;
			auto                     *responses   = reinterpret_cast<struct pam_response *>(0x1);
			const auto                conversation =
			    ObservedPromptConversationTestAccess::OverrideConversation(*wrapper);
			ok &= Expect(conversation.conv(1, &message_ptr, &responses, conversation.appdata_ptr) ==
			                 PAM_SUCCESS,
			             "non-secret prompt delegates successfully");
			ok &= Expect(observer.begin_calls == 0 && observer.end_calls == 0,
			             "non-secret prompt does not create generation");
			ok &= Expect(conversation.conv(0, &message_ptr, &responses, conversation.appdata_ptr) ==
			                 PAM_CONV_ERR,
			             "zero-message conversation is rejected");
			ok &= Expect(responses == nullptr, "zero-message conversation clears response");
		}

		for (const auto mode : {ResponseMode::kNull, ResponseMode::kAllocated,
		                        ResponseMode::kPartial, ResponseMode::kThrowAfterAllocation}) {
			OperationState operations;
			DispatchState  dispatch{.mode = mode, .result = PAM_CONV_ERR};
			ObserverState  observer;
			auto           wrapper = MakeDispatchWrapper(&operations, &dispatch, &observer);
			std::array<struct pam_message, 3>         messages{};
			std::array<const struct pam_message *, 3> message_ptrs{};
			MakeMessages(&messages, &message_ptrs);
			auto      *responses = reinterpret_cast<struct pam_response *>(0x1);
			const auto conversation =
			    ObservedPromptConversationTestAccess::OverrideConversation(*wrapper);
			const int result =
			    conversation.conv(3, message_ptrs.data(), &responses, conversation.appdata_ptr);

			ok &= Expect(result == PAM_CONV_ERR,
			             "observed delegated failure preserves PAM error result");
			ok &= Expect(responses == nullptr,
			             "observed delegated failure always clears response ownership");
			ok &= Expect(observer.begin_calls == 1 && observer.end_calls == 1,
			             "delegated failure or exception closes active prompt generation");
		}

		{
			OperationState operations;
			DispatchState  dispatch{.mode = ResponseMode::kAllocated, .result = PAM_CONV_ERR};
			ObserverState  observer;
			auto           wrapper = MakeDispatchWrapper(&operations, &dispatch, &observer);
			std::array<struct pam_message, 3>         messages{};
			std::array<const struct pam_message *, 3> message_ptrs{};
			MakeMessages(&messages, &message_ptrs);
			const auto conversation =
			    ObservedPromptConversationTestAccess::OverrideConversation(*wrapper);

			ok &= Expect(conversation.conv(3, message_ptrs.data(), nullptr,
			                               conversation.appdata_ptr) == PAM_CONV_ERR,
			             "observed dispatcher rejects null output pointer");
			message_ptrs[1] = nullptr;
			auto *responses = reinterpret_cast<struct pam_response *>(0x1);
			ok &= Expect(conversation.conv(3, message_ptrs.data(), &responses,
			                               conversation.appdata_ptr) == PAM_CONV_ERR,
			             "observed dispatcher rejects invalid message batch");
			ok &= Expect(responses == nullptr, "observed invalid message batch leaves output null");
		}

		return ok;
	}

	auto CallLate(const struct pam_conv &conversation) -> bool {
		const struct pam_message  message{.msg_style = PAM_TEXT_INFO, .msg = "late"};
		const struct pam_message *message_ptr = &message;
		auto                     *response    = reinterpret_cast<struct pam_response *>(0x1);
		return conversation.conv(1, &message_ptr, &response, conversation.appdata_ptr) ==
		           PAM_CONV_ERR &&
		       response == nullptr;
	}

	auto TestConstructorAndInstallGuards() -> bool {
		bool                       ok = true;
		OperationState             operations;
		ObserverState              observer;
		const SecretPromptObserver valid_observer{
		    .context = &observer, .begin = BeginPrompt, .end = EndPrompt};
		const auto make = [&](pam_handle_t *pamh, SecretPromptObserver prompt_observer,
		                      ObservedPromptConversationTestAccess::InjectedOperations injected)
		    -> std::unique_ptr<ObservedPromptConversation> {
			return ObservedPromptConversationTestAccess::Create(pamh, prompt_observer, injected);
		};

		ok &= Expect(
		    !make(
		         nullptr, valid_observer,
		         {.context = &operations, .get_item = InjectedGetItem, .set_item = InjectedSetItem})
		         ->Available(),
		    "null PAM handle makes observed wrapper unavailable");
		ok &= Expect(
		    !make(
		         reinterpret_cast<pam_handle_t *>(0x1),
		         {.context = &observer, .begin = nullptr, .end = EndPrompt},
		         {.context = &operations, .get_item = InjectedGetItem, .set_item = InjectedSetItem})
		         ->Available(),
		    "missing observer begin callback makes wrapper unavailable");
		ok &= Expect(
		    !make(
		         reinterpret_cast<pam_handle_t *>(0x1),
		         {.context = &observer, .begin = BeginPrompt, .end = nullptr},
		         {.context = &operations, .get_item = InjectedGetItem, .set_item = InjectedSetItem})
		         ->Available(),
		    "missing observer end callback makes wrapper unavailable");
		ok &=
		    Expect(!make(reinterpret_cast<pam_handle_t *>(0x1), valid_observer,
		                 {.context = &operations, .get_item = nullptr, .set_item = InjectedSetItem})
		                ->Available(),
		           "missing PAM get-item operation makes wrapper unavailable");

		operations.get_result = PAM_SYSTEM_ERR;
		ok &= Expect(
		    !make(
		         reinterpret_cast<pam_handle_t *>(0x1), valid_observer,
		         {.context = &operations, .get_item = InjectedGetItem, .set_item = InjectedSetItem})
		         ->Available(),
		    "PAM conversation lookup failure makes wrapper unavailable");
		operations.get_result       = PAM_SUCCESS;
		operations.return_null_item = true;
		ok &= Expect(
		    !make(
		         reinterpret_cast<pam_handle_t *>(0x1), valid_observer,
		         {.context = &operations, .get_item = InjectedGetItem, .set_item = InjectedSetItem})
		         ->Available(),
		    "null PAM conversation item makes wrapper unavailable");
		operations.return_null_item = false;
		operations.original         = {};
		ok &= Expect(
		    !make(
		         reinterpret_cast<pam_handle_t *>(0x1), valid_observer,
		         {.context = &operations, .get_item = InjectedGetItem, .set_item = InjectedSetItem})
		         ->Available(),
		    "null original conversation callback makes wrapper unavailable");

		operations.original = {.conv = DelegatedConversation, .appdata_ptr = nullptr};
		auto no_set =
		    make(reinterpret_cast<pam_handle_t *>(0x1), valid_observer,
		         {.context = &operations, .get_item = InjectedGetItem, .set_item = nullptr});
		ok &= Expect(no_set->Available() && no_set->Install() == PAM_SYSTEM_ERR,
		             "missing PAM set-item operation rejects install");
		operations.set_results[0] = PAM_SYSTEM_ERR;
		auto failed_set           = make(
		    reinterpret_cast<pam_handle_t *>(0x1), valid_observer,
		    {.context = &operations, .get_item = InjectedGetItem, .set_item = InjectedSetItem});
		ok &= Expect(failed_set->Available() && failed_set->Install() == PAM_SYSTEM_ERR,
		             "PAM conversation install failure is returned");
		return ok;
	}

	auto TestDestroyedInstalledWrapperFailsClosed() -> bool {
		OperationState operations;
		DispatchState  dispatch;
		ObserverState  observer;
		auto           wrapper = MakeDispatchWrapper(&operations, &dispatch, &observer);
		bool           ok = Expect(wrapper->Install() == PAM_SUCCESS,
		                           "installed observed wrapper prepares unsafe-destruction case");
		const auto installed = ObservedPromptConversationTestAccess::OverrideConversation(*wrapper);
		wrapper.reset();
		return ok && Expect(CallLate(installed),
		                    "destroyed installed wrapper retains fail-closed callback context");
	}

	auto TestRestoreResult(ConversationRestoreResult expected, std::array<int, 3> set_results,
	                       const char *label) -> bool {
		OperationState operations{.set_results = set_results};
		DispatchState  dispatch;
		ObserverState  observer;
		auto           wrapper = MakeDispatchWrapper(&operations, &dispatch, &observer);
		bool ok = Expect(wrapper->Available(), std::string(label) + ": wrapper is available") &&
		          Expect(wrapper->Install() == PAM_SUCCESS,
		                 std::string(label) + ": wrapper installs through injected PAM operation");
		const auto override_conversation =
		    ObservedPromptConversationTestAccess::OverrideConversation(*wrapper);
		const auto result = wrapper->RestoreOriginal();
		ok &= Expect(result == expected, std::string(label) + ": returns explicit restore result");
		ok &= Expect(wrapper->RestoreOriginal() == ConversationRestoreResult::kOriginalRestored,
		             std::string(label) + ": repeated restore is already complete");
		const auto restored_conversation = operations.last_set;
		const int  calls_before_destroy  = operations.set_calls;
		wrapper.reset();
		ok &= Expect(operations.set_calls == calls_before_destroy,
		             std::string(label) + ": destructor performs no PAM operation");

		if (expected == ConversationRestoreResult::kOriginalRestored) {
			ok &= Expect(restored_conversation.conv == DelegatedConversation,
			             std::string(label) + ": original callback is restored");
		} else {
			ok &= Expect(CallLate(restored_conversation),
			             std::string(label) + ": installed fail-closed callback rejects late call");
		}
		if (expected == ConversationRestoreResult::kUnsafe) {
			ok &= Expect(CallLate(override_conversation),
			             std::string(label) + ": quarantined callback rejects late call");
			ok &= Expect(observer.begin_calls == 0 && observer.end_calls == 0,
			             std::string(label) + ": destroyed observer context is never reached");
		}
		return ok;
	}

}  // namespace

auto main() -> int {
	bool ok = true;
	ok &= TestResponseCleanupContract();
	ok &= TestConstructorAndInstallGuards();
	ok &= TestDestroyedInstalledWrapperFailsClosed();
	ok &= TestRestoreResult(ConversationRestoreResult::kOriginalRestored,
	                        {PAM_SUCCESS, PAM_SUCCESS, PAM_SUCCESS}, "original restoration");
	ok &= TestRestoreResult(ConversationRestoreResult::kFailClosedInstalled,
	                        {PAM_SUCCESS, PAM_SYSTEM_ERR, PAM_SUCCESS}, "fail-closed restoration");
	ok &= TestRestoreResult(ConversationRestoreResult::kUnsafe,
	                        {PAM_SUCCESS, PAM_SYSTEM_ERR, PAM_SYSTEM_ERR}, "unsafe restoration");
	return ok ? 0 : 1;
}
