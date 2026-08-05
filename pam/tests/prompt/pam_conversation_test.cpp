#include "prompt/conversation_response.hpp"
#include "prompt/pam_conversation.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

#include <security/pam_appl.h>

struct pam_handle {
	int marker = 0;
};

struct PamGetItemState {
	pam_handle_t *handle    = nullptr;
	int           result    = PAM_SUCCESS;
	int           item_type = 0;
	const void   *item      = nullptr;
	int           calls     = 0;
};

struct ConversationCleanupState {
	int                  calls     = 0;
	int                  count     = -1;
	struct pam_response *responses = nullptr;
	std::string          secret;
};

PamGetItemState          *pam_get_item_state         = nullptr;
ConversationCleanupState *conversation_cleanup_state = nullptr;

extern "C" auto pam_get_item(const pam_handle_t *pamh, int item_type, const void **item) -> int {
	if (pam_get_item_state == nullptr || pamh != pam_get_item_state->handle) {
		return PAM_SYSTEM_ERR;
	}
	++pam_get_item_state->calls;
	pam_get_item_state->item_type = item_type;
	if (item != nullptr) {
		*item = pam_get_item_state->item;
	}
	return pam_get_item_state->result;
}

namespace howdy::pam {

	void secure_free_conversation_responses(struct pam_response **responses, int count) noexcept {
		if (conversation_cleanup_state != nullptr) {
			++conversation_cleanup_state->calls;
			conversation_cleanup_state->count = count;
			if (responses != nullptr && *responses != nullptr && count > 0 &&
			    (*responses)[0].resp != nullptr) {
				conversation_cleanup_state->responses = *responses;
				conversation_cleanup_state->secret    = (*responses)[0].resp;
			}
		}

		if (responses == nullptr || *responses == nullptr) {
			return;
		}
		auto *owned_responses = *responses;
		*responses            = nullptr;
		for (int index = 0; index < count; ++index) {
			if (owned_responses[index].resp == nullptr) {
				continue;
			}
			explicit_bzero(owned_responses[index].resp, std::strlen(owned_responses[index].resp));
			std::free(owned_responses[index].resp);
			owned_responses[index].resp = nullptr;
		}
		std::free(owned_responses);
	}

}  // namespace howdy::pam

namespace {

	using howdy::pam::ConversationMessage;
	using howdy::pam::PamConversation;
	using howdy::test::expect;

	enum class ResponseMode : std::uint8_t {
		None,
		One,
		ThrowAfterAllocation,
	};

	struct ConversationState {
		int                  result        = PAM_SUCCESS;
		int                  calls         = 0;
		int                  message_count = 0;
		int                  message_style = 0;
		std::string          message_text;
		void                *observed_appdata_ptr   = nullptr;
		ResponseMode         response_mode          = ResponseMode::None;
		bool                 response_was_allocated = false;
		struct pam_response *allocated_responses    = nullptr;
	};

	auto test_conversation(int num_msg, const struct pam_message **messages,
	                       struct pam_response **responses, void *appdata_ptr) -> int {
		auto *state = static_cast<ConversationState *>(appdata_ptr);
		if (state == nullptr || messages == nullptr || messages[0] == nullptr ||
		    responses == nullptr || num_msg != 1) {
			return PAM_CONV_ERR;
		}

		++state->calls;
		state->message_count        = num_msg;
		state->message_style        = messages[0]->msg_style;
		state->message_text         = messages[0]->msg == nullptr ? "" : messages[0]->msg;
		state->observed_appdata_ptr = appdata_ptr;
		*responses                  = nullptr;

		if (state->response_mode != ResponseMode::None) {
			*responses = static_cast<struct pam_response *>(calloc(1, sizeof(struct pam_response)));
			if (*responses == nullptr) {
				return PAM_BUF_ERR;
			}

			(*responses)->resp = strdup("adapter-secret");
			if ((*responses)->resp == nullptr) {
				free(*responses);
				*responses = nullptr;
				return PAM_BUF_ERR;
			}
			state->response_was_allocated = true;
			state->allocated_responses    = *responses;
			if (state->response_mode == ResponseMode::ThrowAfterAllocation) {
				throw std::runtime_error("conversation callback failure");
			}
		}

		return state->result;
	}

	auto expect_acquire_validation() -> bool {
		bool                  ok = true;
		PamConversation       conversation;
		ConversationState     state;
		struct pam_handle     pam_handle;
		const struct pam_conv original{.conv = test_conversation, .appdata_ptr = &state};
		PamGetItemState       get_item_state{.handle = &pam_handle};
		pam_get_item_state = &get_item_state;

		ok &= expect(PamConversation::acquire(nullptr, &conversation) == PAM_SYSTEM_ERR,
		             "null PAM handle fails acquisition");
		ok &= expect(PamConversation::acquire(&pam_handle, nullptr) == PAM_SYSTEM_ERR,
		             "null output fails acquisition");

		get_item_state.result = PAM_SYSTEM_ERR;
		ok &= expect(PamConversation::acquire(&pam_handle, &conversation) == PAM_SYSTEM_ERR,
		             "pam_get_item failure propagates status");
		ok &= expect(get_item_state.calls == 1 && get_item_state.item_type == PAM_CONV,
		             "acquisition requests PAM_CONV");

		get_item_state.result = PAM_SUCCESS;
		get_item_state.item   = nullptr;
		ok &= expect(PamConversation::acquire(&pam_handle, &conversation) == PAM_SYSTEM_ERR,
		             "null PAM conversation item fails acquisition");

		const struct pam_conv unavailable{.conv = nullptr, .appdata_ptr = &state};
		get_item_state.item = &unavailable;
		ok &= expect(PamConversation::acquire(&pam_handle, &conversation) == PAM_SYSTEM_ERR,
		             "null PAM conversation callback fails acquisition");

		get_item_state.item = &original;
		ok &= expect(PamConversation::acquire(&pam_handle, &conversation) == PAM_SUCCESS,
		             "acquires valid PAM conversation");
		return ok;
	}

	auto expect_throwing_callback_cleanup() -> bool {
		bool                     ok = true;
		ConversationState        state{.response_mode = ResponseMode::ThrowAfterAllocation};
		ConversationCleanupState cleanup;
		struct pam_handle        pam_handle;
		const struct pam_conv    original{.conv = test_conversation, .appdata_ptr = &state};
		PamGetItemState          get_item_state{.handle = &pam_handle, .item = &original};
		pam_get_item_state         = &get_item_state;
		conversation_cleanup_state = &cleanup;

		PamConversation conversation;
		ok &= expect(PamConversation::acquire(&pam_handle, &conversation) == PAM_SUCCESS,
		             "throw test acquires PAM conversation");
		const ConversationMessage message{.style = PAM_PROMPT_ECHO_OFF, .text = "throwing prompt"};
		bool                      exception_escaped = false;
		int                       send_result       = PAM_SUCCESS;
		try {
			send_result = conversation.send(message);
		} catch (...) {
			exception_escaped = true;
		}
		ok &= expect(!exception_escaped, "callback exception does not escape send");
		ok &= expect(send_result == PAM_CONV_ERR, "throwing callback returns PAM_CONV_ERR");
		ok &= expect(state.response_was_allocated,
		             "throwing callback allocates response before cleanup");
		ok &= expect(cleanup.calls == 1, "throwing callback invokes cleanup once");
		ok &= expect(cleanup.count == 1, "throwing callback cleanup receives one response");
		ok &= expect(cleanup.responses == state.allocated_responses,
		             "cleanup receives allocated response");
		ok &= expect(cleanup.secret == "adapter-secret",
		             "cleanup receives allocated response secret");
		conversation_cleanup_state = nullptr;
		return ok;
	}

	auto expect_dispatch() -> bool {
		bool                  ok = true;
		ConversationState     state;
		struct pam_handle     pam_handle;
		const struct pam_conv original{.conv = test_conversation, .appdata_ptr = &state};
		PamGetItemState       get_item_state{.handle = &pam_handle, .item = &original};
		pam_get_item_state = &get_item_state;

		PamConversation conversation;
		ok &= expect(PamConversation::acquire(&pam_handle, &conversation) == PAM_SUCCESS,
		             "acquires PAM conversation");

		state.response_mode = ResponseMode::None;
		state.result        = PAM_SUCCESS;
		const ConversationMessage info_message{.style = PAM_TEXT_INFO,
		                                       .text  = "exact adapter text"};
		ok &= expect(conversation.send(info_message) == PAM_SUCCESS,
		             "accepts callback without response");
		ok &= expect(state.calls == 1 && state.message_count == 1 &&
		                 state.message_style == PAM_TEXT_INFO &&
		                 state.message_text == "exact adapter text",
		             "forwards one exact typed message");
		ok &= expect(state.observed_appdata_ptr == &state, "preserves original appdata pointer");

		state.response_mode          = ResponseMode::One;
		state.response_was_allocated = false;
		state.result                 = PAM_SUCCESS;
		const ConversationMessage secret_message{.style = PAM_PROMPT_ECHO_OFF,
		                                         .text  = "secret prompt"};
		ok &= expect(conversation.send(secret_message) == PAM_SUCCESS,
		             "accepts callback with one response");
		ok &= expect(state.response_was_allocated, "callback returned one response");
		ok &= expect(state.message_style == PAM_PROMPT_ECHO_OFF &&
		                 state.message_text == "secret prompt",
		             "forwards prompt style and text");

		state.response_was_allocated = false;
		state.result                 = PAM_CONV_ERR;
		const ConversationMessage error_message{.style = PAM_ERROR_MSG, .text = "failed callback"};
		ok &= expect(conversation.send(error_message) == PAM_CONV_ERR,
		             "propagates callback failure status");
		ok &= expect(state.response_was_allocated, "cleans response returned on callback failure");
		return ok;
	}

}  // namespace

auto main() -> int {
	const bool acquire_ok  = expect_acquire_validation();
	const bool dispatch_ok = expect_dispatch();
	const bool throw_ok    = expect_throwing_callback_cleanup();
	return acquire_ok && dispatch_ok && throw_ok ? 0 : 1;
}
