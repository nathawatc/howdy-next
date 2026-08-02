#include "prompt/conversation_response.hpp"

#include "conversation_response_internal.hpp"

#include <cstdlib>
#include <cstring>

namespace {
	void erase_response(howdy::pam::detail::ConversationResponseAllocation allocation,
	                    std::size_t                                        length) noexcept {
		explicit_bzero(allocation.pointer, length);
	}

	void release_response(howdy::pam::detail::ConversationResponseAllocation allocation) noexcept {
		std::free(allocation.pointer);
	}
}  // namespace

namespace howdy::pam::detail {

	void secure_free_conversation_responses(struct pam_response **responses, int count,
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
	void secure_free_conversation_responses(struct pam_response **responses, int count) noexcept {
		detail::secure_free_conversation_responses(
		    responses, count, {.erase = erase_response, .release = release_response});
	}
}  // namespace howdy::pam
