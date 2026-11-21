#pragma once

#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <security/pam_appl.h>

namespace howdy::pam {

	__attribute__((visibility("hidden"))) inline auto
	FailClosedConversationDispatch(int /*num_msg*/, const struct pam_message ** /*messages*/,
	                               struct pam_response **response, void * /*appdata_ptr*/) -> int {
		if (response != nullptr) {
			*response = nullptr;
		}
		return PAM_CONV_ERR;
	}

	template <typename Context>
	[[nodiscard]] auto QuarantineUnsafeCallbackContext(std::unique_ptr<Context> &context) noexcept
	    -> bool {
		static std::mutex                            quarantine_mutex;
		static std::vector<std::unique_ptr<Context>> quarantine;
		try {
			std::scoped_lock const lock(quarantine_mutex);
			quarantine.push_back(std::move(context));
			return true;
		} catch (...) {
			// Allocation failure cannot make stale PAM callback safe to free. Retain tiny
			// fail-closed context for process lifetime and report explicit ownership fallback.
			[[maybe_unused]] auto const *retained_context = context.release();
			return false;
		}
	}

}  // namespace howdy::pam
