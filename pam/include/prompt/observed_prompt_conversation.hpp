#pragma once

#include "prompt/conversation_restore.hpp"

#include <cstdint>
#include <memory>

#include <security/pam_appl.h>

namespace howdy::pam {
	class ObservedPromptConversationTestAccess;

	using SecretPromptGeneration = std::uint64_t;
	using SecretPromptBeginFn    = SecretPromptGeneration (*)(void *context);
	using SecretPromptEndFn      = void (*)(void *context, SecretPromptGeneration generation);

	struct SecretPromptObserver {
		void               *context = nullptr;
		SecretPromptBeginFn begin   = nullptr;
		SecretPromptEndFn   end     = nullptr;
	};

	class SecretPromptConversation {
	public:
		virtual ~SecretPromptConversation() = default;

		[[nodiscard]] virtual auto Available() const -> bool                               = 0;
		virtual auto               Install() -> int                                        = 0;
		virtual auto               RestoreOriginal() noexcept -> ConversationRestoreResult = 0;
	};

	class ObservedPromptConversation final : public SecretPromptConversation {
	public:
		ObservedPromptConversation(pam_handle_t *pamh, SecretPromptObserver observer);
		~ObservedPromptConversation() override;

		ObservedPromptConversation(const ObservedPromptConversation &)                     = delete;
		auto operator=(const ObservedPromptConversation &) -> ObservedPromptConversation & = delete;

		[[nodiscard]] auto Available() const -> bool override;
		auto               Install() -> int override;
		auto               RestoreOriginal() noexcept -> ConversationRestoreResult override;

	private:
		friend class ObservedPromptConversationTestAccess;

		struct Operations {
			void *context                      = nullptr;
			int (*get_item)(void *context, pam_handle_t *pamh, int item_type,
			                const void **item) = nullptr;
			int (*set_item)(void *context, pam_handle_t *pamh, int item_type,
			                const void *item)  = nullptr;
		};
		struct DispatchContext;

		ObservedPromptConversation(pam_handle_t *pamh, SecretPromptObserver observer,
		                           Operations operations);
		static auto ProductionOperations() -> Operations;

		static auto Dispatch(int num_msg, const struct pam_message **messages,
		                     struct pam_response **response, void *appdata_ptr) -> int;
		void        RetainUnsafeContext() noexcept;

		pam_handle_t                    *pamh_ = nullptr;
		Operations                       operations_{};
		std::unique_ptr<DispatchContext> context_;
		struct pam_conv                  override_conv_{};
		bool                             installed_ = false;
	};

}  // namespace howdy::pam
