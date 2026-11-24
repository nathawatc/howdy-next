#pragma once

#include "prompt/conversation_restore.hpp"

#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <security/pam_appl.h>
#include <security/pam_ext.h>

#include <sys/syslog.h>

namespace howdy::pam {

	__attribute__((visibility("hidden"))) inline auto
	FailClosedConversationDispatch(int /*num_msg*/, const struct pam_message ** /*messages*/,
	                               struct pam_response **response, void * /*appdata_ptr*/) -> int {
		if (response != nullptr) {
			*response = nullptr;
		}
		return PAM_CONV_ERR;
	}

	inline constexpr struct pam_conv kFailClosedConversation{
	    .conv        = FailClosedConversationDispatch,
	    .appdata_ptr = nullptr,
	};

	template <typename SetItemFn, typename RetainUnsafeFn>
	[[nodiscard]] auto RestorePamConversation(pam_handle_t *pamh, bool &installed,
	                                          const struct pam_conv *original_conv,
	                                          const SetItemFn       &set_item,
	                                          const RetainUnsafeFn  &retain_unsafe) noexcept
	    -> ConversationRestoreResult {
		if (!installed) {
			return ConversationRestoreResult::kOriginalRestored;
		}
		installed = false;
		if (pamh == nullptr) {
			pam_syslog(pamh, LOG_CRIT, "Cannot restore PAM conversation: null PAM handle");
			retain_unsafe();
			return ConversationRestoreResult::kUnsafe;
		}
		if (original_conv != nullptr) {
			const int restore_result = set_item(pamh, PAM_CONV, original_conv);
			if (restore_result == PAM_SUCCESS) {
				return ConversationRestoreResult::kOriginalRestored;
			}
			pam_syslog(pamh, LOG_CRIT, "Failed to restore original PAM conversation: %d",
			           restore_result);
		}
		const int fail_closed_result = set_item(pamh, PAM_CONV, &kFailClosedConversation);
		if (fail_closed_result == PAM_SUCCESS) {
			return ConversationRestoreResult::kFailClosedInstalled;
		}
		pam_syslog(pamh, LOG_CRIT, "Failed to install fail-closed PAM conversation: %d",
		           fail_closed_result);
		retain_unsafe();
		return ConversationRestoreResult::kUnsafe;
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
