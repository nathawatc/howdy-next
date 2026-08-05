#include "prompt/pam_conversation.hpp"

#include "prompt/conversation_response.hpp"

#include <syslog.h>

namespace {

	class ConversationResponseOwner final {
	public:
		ConversationResponseOwner() = default;

		~ConversationResponseOwner() {
			howdy::pam::secure_free_conversation_responses(&responses_, 1);
		}

		ConversationResponseOwner(const ConversationResponseOwner &)                     = delete;
		auto operator=(const ConversationResponseOwner &) -> ConversationResponseOwner & = delete;

		[[nodiscard]] auto pointer() -> struct pam_response ** {
			return &responses_;
		}

	private:
		struct pam_response *responses_ = nullptr;
	};

}  // namespace

namespace howdy::pam {

	PamConversation::PamConversation(struct pam_conv callback)
	    : callback_(callback) {}

	auto PamConversation::acquire(pam_handle_t *pamh, PamConversation *output) noexcept -> int {
		if (pamh == nullptr || output == nullptr) {
			syslog(LOG_ERR, "Failed to acquire conversation");
			return PAM_SYSTEM_ERR;
		}

		const void *item   = nullptr;
		const int   status = pam_get_item(pamh, PAM_CONV, &item);
		if (status != PAM_SUCCESS) {
			syslog(LOG_ERR, "Failed to acquire conversation");
			return status;
		}
		if (item == nullptr) {
			syslog(LOG_ERR, "PAM conversation is not available");
			return PAM_SYSTEM_ERR;
		}

		const auto *callback = static_cast<const struct pam_conv *>(item);
		if (callback->conv == nullptr) {
			syslog(LOG_ERR, "PAM conversation is not available");
			return PAM_SYSTEM_ERR;
		}

		*output = PamConversation(*callback);
		return PAM_SUCCESS;
	}

	auto PamConversation::send(const ConversationMessage &message) const noexcept -> int {
		if (callback_.conv == nullptr) {
			return PAM_SYSTEM_ERR;
		}

		const struct pam_message  pam_message{.msg_style = message.style,
		                                      .msg       = message.text.c_str()};
		const struct pam_message *message_pointer = &pam_message;
		ConversationResponseOwner response_owner;

		try {
			return callback_.conv(1, &message_pointer, response_owner.pointer(),
			                      callback_.appdata_ptr);
		} catch (...) {
			return PAM_CONV_ERR;
		}
	}

}  // namespace howdy::pam
