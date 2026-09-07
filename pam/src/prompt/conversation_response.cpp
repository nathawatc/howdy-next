#include "prompt/conversation_response.hpp"

#include "prompt/conversation_response/internal.hpp"

#include <cstdlib>
#include <cstring>

namespace {
	void EraseResponse(howdy::pam::detail::ConversationResponseAllocation allocation,
	                   std::size_t                                        length) noexcept {
		explicit_bzero(allocation.pointer, length);
	}

	void ReleaseResponse(howdy::pam::detail::ConversationResponseAllocation allocation) noexcept {
		std::free(allocation.pointer);
	}
}  // namespace

namespace howdy::pam::detail {

	void SecureFreeConversationResponses(struct pam_response **responses, int count,
	                                     ConversationResponseOperations operations) noexcept {
		if (responses == nullptr || *responses == nullptr || operations.erase == nullptr ||
		    operations.release == nullptr) {
			return;
		}
		auto *owned_responses = *responses;
		*responses            = nullptr;
		for (int index = 0; index < count; ++index) {
			char *secret = owned_responses[index].resp;
			if (secret == nullptr) {
				continue;
			}
			const ConversationResponseAllocation allocation{.context = operations.context,
			                                                .pointer = secret};
			operations.erase(allocation, std::strlen(secret));
			operations.release(allocation);
			owned_responses[index].resp = nullptr;
		}
		operations.release({.context = operations.context, .pointer = owned_responses});
	}

}  // namespace howdy::pam::detail

namespace howdy::pam {
	void SecureFreeConversationResponses(struct pam_response **responses, int count) noexcept {
		detail::SecureFreeConversationResponses(
		    responses, count, {.erase = EraseResponse, .release = ReleaseResponse});
	}
}  // namespace howdy::pam
