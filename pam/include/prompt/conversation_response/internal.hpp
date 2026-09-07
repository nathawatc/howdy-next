#pragma once

#include <cstddef>

#include <security/pam_appl.h>

namespace howdy::pam::detail {
	struct ConversationResponseAllocation {
		void *context = nullptr;
		void *pointer = nullptr;
	};

	struct ConversationResponseOperations {
		void *context                                                       = nullptr;
		void (*erase)(ConversationResponseAllocation allocation,
		              std::size_t                    length) noexcept       = nullptr;
		void (*release)(ConversationResponseAllocation allocation) noexcept = nullptr;
	};

	void SecureFreeConversationResponses(struct pam_response **responses, int count,
	                                     ConversationResponseOperations operations) noexcept;
}  // namespace howdy::pam::detail
