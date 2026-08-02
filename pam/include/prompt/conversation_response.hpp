#pragma once

#include <security/pam_appl.h>

namespace howdy::pam {
	void secure_free_conversation_responses(struct pam_response **responses, int count) noexcept;

}  // namespace howdy::pam
