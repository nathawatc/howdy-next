#pragma once

#include <string>

#include <security/pam_appl.h>

namespace howdy::pam {

	struct ConversationMessage {
		int         style;
		std::string text;
	};

	class PamConversation {
	public:
		PamConversation() = default;

		[[nodiscard]] static auto acquire(pam_handle_t *pamh, PamConversation *output) noexcept
		    -> int;

		[[nodiscard]] auto send(const ConversationMessage &message) const noexcept -> int;

	private:
		explicit PamConversation(struct pam_conv callback);

		struct pam_conv callback_{};
	};

}  // namespace howdy::pam
