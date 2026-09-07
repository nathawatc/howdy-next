#pragma once

#include <security/pam_appl.h>

namespace howdy::pam {
	void SecureFreeConversationResponses(struct pam_response **responses, int count) noexcept;

}  // namespace howdy::pam
