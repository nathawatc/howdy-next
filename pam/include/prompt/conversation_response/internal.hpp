#pragma once

#include <cstddef>
#include <span>

#include <security/pam_appl.h>

namespace howdy::pam::detail {
	struct ConversationResponseAllocation {
		void *context = nullptr;
		void *pointer = nullptr;
	};

	struct ConversationResponseOperations {
		void *context                                                       = nullptr;
		void (*erase)(void *context, std::span<char> bytes) noexcept        = nullptr;
		void (*release)(ConversationResponseAllocation allocation) noexcept = nullptr;
	};

	void SecureFreeConversationResponses(struct pam_response **responses, int count,
	                                     ConversationResponseOperations operations) noexcept;
	void SecureFreeConversationResponses(struct pam_response          **responses,
	                                     std::span<const std::size_t>   lengths,
	                                     ConversationResponseOperations operations) noexcept;
}  // namespace howdy::pam::detail
