#pragma once

#include <cstddef>
#include <span>

#include <security/pam_appl.h>

namespace howdy::pam {
	void SecureFreeConversationResponses(struct pam_response **responses, int count) noexcept;
	void SecureFreeConversationResponses(struct pam_response        **responses,
	                                     std::span<const std::size_t> lengths) noexcept;

}  // namespace howdy::pam
