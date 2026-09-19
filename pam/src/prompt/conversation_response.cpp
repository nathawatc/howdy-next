#include "prompt/conversation_response.hpp"

#include "prompt/conversation_response/internal.hpp"

#include <cstdlib>
#include <cstring>
#include <span>

#include <string.h>

namespace {
	void EraseResponse(void *context, std::span<char> bytes) noexcept {
		(void)context;
		explicit_bzero(bytes.data(), bytes.size());
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
		auto *owned_responses     = *responses;
		*responses                = nullptr;
		const auto response_count = count > 0 ? static_cast<std::size_t>(count) : 0U;
		for (auto &owned_response : std::span(owned_responses, response_count)) {
			char *secret = owned_response.resp;
			if (secret == nullptr) {
				continue;
			}
			operations.erase(operations.context, {secret, std::strlen(secret)});
			operations.release({.context = operations.context, .pointer = secret});
			owned_response.resp = nullptr;
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
