#include "prompt/observed_prompt_conversation.hpp"

#include "prompt/conversation_response.hpp"

#include <atomic>
#include <exception>
#include <memory>
#include <mutex>
#include <syslog.h>
#include <vector>

namespace {
	auto ProductionGetItem(void * /*context*/, pam_handle_t *pamh, int item_type, const void **item)
	    -> int {
		return pam_get_item(pamh, item_type, item);
	}

	auto ProductionSetItem(void * /*context*/, pam_handle_t *pamh, int item_type, const void *item)
	    -> int {
		return pam_set_item(pamh, item_type, item);
	}

	auto ObservedPromptFailClosedDispatch(int /*num_msg*/, const struct pam_message ** /*messages*/,
	                                      struct pam_response **response, void * /*appdata_ptr*/)
	    -> int {
		if (response != nullptr) {
			*response = nullptr;
		}
		return PAM_CONV_ERR;
	}

}  // namespace

namespace howdy::pam {

	struct ObservedPromptConversation::DispatchContext {
		struct pam_conv      original{};
		SecretPromptObserver observer{};
		std::atomic<bool>    fail_closed{false};
	};

	ObservedPromptConversation::ObservedPromptConversation(pam_handle_t        *pamh,
	                                                       SecretPromptObserver observer)
	    : ObservedPromptConversation(pamh, observer, ProductionOperations()) {}

	ObservedPromptConversation::ObservedPromptConversation(pam_handle_t        *pamh,
	                                                       SecretPromptObserver observer,
	                                                       Operations           operations)
	    : pamh_(pamh)
	    , operations_(operations) {
		const void *conversation_item = nullptr;
		if (pamh_ == nullptr || observer.begin == nullptr || observer.end == nullptr ||
		    operations_.get_item == nullptr ||
		    operations_.get_item(operations_.context, pamh_, PAM_CONV, &conversation_item) !=
		        PAM_SUCCESS ||
		    conversation_item == nullptr) {
			return;
		}

		const auto original = *static_cast<const struct pam_conv *>(conversation_item);
		if (original.conv == nullptr) {
			return;
		}
		context_           = std::make_unique<DispatchContext>();
		context_->original = original;
		context_->observer = observer;
		override_conv_     = {.conv = Dispatch, .appdata_ptr = context_.get()};
	}

	auto ObservedPromptConversation::ProductionOperations() -> Operations {
		return {
		    .get_item = ProductionGetItem,
		    .set_item = ProductionSetItem,
		};
	}

	ObservedPromptConversation::~ObservedPromptConversation() {
		if (installed_ && context_ != nullptr) {
			RetainUnsafeContext();
		}
	}

	auto ObservedPromptConversation::Available() const -> bool {
		return pamh_ != nullptr && context_ != nullptr;
	}

	auto ObservedPromptConversation::Install() -> int {
		if (!Available() || operations_.set_item == nullptr) {
			return PAM_SYSTEM_ERR;
		}
		const int result =
		    operations_.set_item(operations_.context, pamh_, PAM_CONV, &override_conv_);
		if (result == PAM_SUCCESS) {
			installed_ = true;
		}
		return result;
	}

	auto ObservedPromptConversation::Dispatch(int num_msg, const struct pam_message **messages,
	                                          struct pam_response **response, void *appdata_ptr)
	    -> int {
		if (response == nullptr) {
			return PAM_CONV_ERR;
		}
		*response = nullptr;
		try {
			auto *context = static_cast<DispatchContext *>(appdata_ptr);
			if (context == nullptr || num_msg <= 0 || messages == nullptr ||
			    context->fail_closed.load()) {
				return PAM_CONV_ERR;
			}

			bool secret_prompt_observed = false;
			for (int index = 0; index < num_msg; ++index) {
				if (messages[index] == nullptr) {
					return PAM_CONV_ERR;
				}
				secret_prompt_observed |= messages[index]->msg_style == PAM_PROMPT_ECHO_OFF;
			}
			SecretPromptGeneration generation = 0;
			if (secret_prompt_observed) {
				generation = context->observer.begin(context->observer.context);
			}

			struct PromptGenerationScope {
				SecretPromptObserver   observer{};
				SecretPromptGeneration generation = 0;

				~PromptGenerationScope() noexcept {
					if (generation != 0) {
						try {
							observer.end(observer.context, generation);
						} catch (...) {
							syslog(LOG_ERR, "Secret prompt generation cleanup failed");
						}
					}
				}
			} generation_scope{.observer = context->observer, .generation = generation};

			const int result =
			    context->original.conv(num_msg, messages, response, context->original.appdata_ptr);
			if (result != PAM_SUCCESS) {
				SecureFreeConversationResponses(response, num_msg);
			}
			return result;
		} catch (const std::exception &error) {
			syslog(LOG_ERR, "Unhandled C++ exception in observed PAM conversation: %s",
			       error.what());
		} catch (...) {
			syslog(LOG_ERR, "Unhandled non-standard exception in observed PAM conversation");
		}
		SecureFreeConversationResponses(response, num_msg);
		return PAM_CONV_ERR;
	}

	void ObservedPromptConversation::RetainUnsafeContext() noexcept {
		context_->fail_closed.store(true);
		static std::mutex                                    quarantine_mutex;
		static std::vector<std::unique_ptr<DispatchContext>> quarantine;
		try {
			std::scoped_lock lock(quarantine_mutex);
			quarantine.push_back(std::move(context_));
		} catch (...) {
			// Allocation failure cannot make stale PAM callback safe to free. Retain tiny
			// fail-closed context for process lifetime and report explicit ownership fallback.
			[[maybe_unused]] auto *retained_context = context_.release();
			syslog(LOG_CRIT, "Observed fail-closed callback context retained outside quarantine");
		}
	}

	auto ObservedPromptConversation::RestoreOriginal() noexcept -> ConversationRestoreResult {
		if (!installed_) {
			return ConversationRestoreResult::kOriginalRestored;
		}
		if (pamh_ != nullptr && context_ != nullptr && operations_.set_item != nullptr &&
		    operations_.set_item(operations_.context, pamh_, PAM_CONV, &context_->original) ==
		        PAM_SUCCESS) {
			installed_ = false;
			return ConversationRestoreResult::kOriginalRestored;
		}

		static const struct pam_conv kFailClosedConv{
		    .conv        = ObservedPromptFailClosedDispatch,
		    .appdata_ptr = nullptr,
		};
		if (pamh_ != nullptr && operations_.set_item != nullptr &&
		    operations_.set_item(operations_.context, pamh_, PAM_CONV, &kFailClosedConv) ==
		        PAM_SUCCESS) {
			installed_ = false;
			return ConversationRestoreResult::kFailClosedInstalled;
		}

		RetainUnsafeContext();
		installed_ = false;
		return ConversationRestoreResult::kUnsafe;
	}

}  // namespace howdy::pam
