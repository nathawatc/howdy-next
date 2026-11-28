#include "config/runtime_config.hpp"
#include "module/auth_flow.hpp"
#include "module/auth_flow_test_groups.hpp"
#include "module/auth_flow_test_support.hpp"
#include "prompt/pam_conversation.hpp"
#include "protocol/compare_exit.hpp"
#include "support/process_status.hpp"
#include "test_support.hpp"

#include <csignal>
#include <cstdlib>
#include <string>

#include <signal.h>

#include <sys/wait.h>

namespace {

	struct AuthTokenOverrideState {
		bool        enabled = false;
		int         result  = PAM_BAD_ITEM;
		const void *token   = nullptr;
	};

	auto AuthTokenOverride() -> AuthTokenOverrideState & {
		static AuthTokenOverrideState state;
		return state;
	}

}  // namespace

extern "C" auto TestPamGetItem(const pam_handle_t *pamh, int item_type, const void **item)
    -> int __asm__("__wrap_pam_get_item");
extern "C" auto RealPamGetItem(const pam_handle_t *pamh, int item_type, const void **item)
    -> int __asm__("__real_pam_get_item");

auto TestPamGetItem(const pam_handle_t *pamh, int item_type, const void **item) -> int {
	const auto &override_state = AuthTokenOverride();
	if (override_state.enabled && item_type == PAM_AUTHTOK && pamh != nullptr && item != nullptr) {
		*item = override_state.token;
		return override_state.result;
	}
	return RealPamGetItem(pamh, item_type, item);
}

namespace howdy::test::auth_flow {

	ScopedAuthTokenItemOverride::ScopedAuthTokenItemOverride(int result, const void *token) noexcept
	    : previous_enabled_(AuthTokenOverride().enabled)
	    , previous_result_(AuthTokenOverride().result)
	    , previous_token_(AuthTokenOverride().token) {
		AuthTokenOverride().enabled = true;
		AuthTokenOverride().result  = result;
		AuthTokenOverride().token   = token;
	}

	ScopedAuthTokenItemOverride::~ScopedAuthTokenItemOverride() {
		AuthTokenOverride().enabled = previous_enabled_;
		AuthTokenOverride().result  = previous_result_;
		AuthTokenOverride().token   = previous_token_;
	}

}  // namespace howdy::test::auth_flow

namespace {
	using howdy::test::auth_flow::ConversationState;
	using howdy::test::auth_flow::EligibilityFlowFixture;
	using howdy::test::auth_flow::ScopedAuthTokenItemOverride;
	using howdy::test::auth_flow::ScopedPamHandle;
	using howdy::test::auth_flow::TestConversation;

	using howdy::test::Expect;

	auto ExpectConversationHelpers() -> bool {
		using howdy::pam::auth_flow::AuthTokenPresent;
		using howdy::pam::auth_flow::ConversationFn;
		using howdy::pam::auth_flow::SendConversationMessage;

		bool                 ok            = true;
		int                  direct_calls  = 0;
		int                  direct_type   = 0;
		int                  direct_result = PAM_CONV_ERR;
		std::string          direct_message;
		const ConversationFn direct_conversation =
		    [&](const howdy::pam::ConversationMessage &message) -> int {
			++direct_calls;
			direct_type    = message.style;
			direct_message = message.text;
			return direct_result;
		};
		SendConversationMessage(nullptr, direct_conversation, PAM_ERROR_MSG, "direct message");
		ok &= Expect(direct_calls == 1 && direct_type == PAM_ERROR_MSG &&
		                 direct_message == "direct message",
		             "message helper invokes conversation despite conversation failure");
		direct_result = PAM_SUCCESS;
		SendConversationMessage(nullptr, direct_conversation, PAM_TEXT_INFO, "successful message");
		ok &= Expect(direct_calls == 2 && direct_type == PAM_TEXT_INFO &&
		                 direct_message == "successful message",
		             "message helper invokes successful conversation");

		ConversationState     state;
		struct pam_conv const conversation{
		    .conv        = TestConversation,
		    .appdata_ptr = &state,
		};
		ScopedPamHandle pam_handle;
		ok &= Expect(pam_handle.Start(&conversation) == PAM_SUCCESS, "starts PAM handle");
		if (pam_handle.Get() == nullptr) {
			return false;
		}

		ok &= Expect(!AuthTokenPresent(nullptr), "null PAM handle reports absent auth token");
		ok &= Expect(!AuthTokenPresent(pam_handle.Get()), "missing auth token is absent");
		{
			const char                       *preset_token = "preset-token";
			const ScopedAuthTokenItemOverride present(PAM_SUCCESS, preset_token);
			ok &= Expect(AuthTokenPresent(pam_handle.Get()), "present PAM_AUTHTOK is detected");
			ok &= Expect(!AuthTokenPresent(nullptr),
			             "null PAM handle still reports absent auth token under override");
		}
		{
			const ScopedAuthTokenItemOverride null_token(PAM_SUCCESS, nullptr);
			ok &= Expect(!AuthTokenPresent(pam_handle.Get()),
			             "PAM_SUCCESS with null PAM_AUTHTOK is absent");
		}
		{
			const char                       *stale_token = "stale-token";
			const ScopedAuthTokenItemOverride error_result(PAM_BAD_ITEM, stale_token);
			ok &= Expect(!AuthTokenPresent(pam_handle.Get()),
			             "non-PAM_SUCCESS pam_get_item fails closed even with non-null item");
		}

		return ok;
	}

	auto ExpectAuthenticationGuards() -> bool {
		EligibilityFlowFixture fixture;
		auto const             dependencies = MakeEligibilityFlowDependencies(&fixture);
		ConversationState      state;
		struct pam_conv const  conversation{
		    .conv        = TestConversation,
		    .appdata_ptr = &state,
		};
		bool ok = true;

		ScopedPamHandle valid_handle;
		ok &= Expect(valid_handle.Start(&conversation) == PAM_SUCCESS,
		             "guard test starts valid PAM handle");
		if (valid_handle.Get() == nullptr) {
			return false;
		}
		ok &= Expect(howdy::pam::auth_flow::IdentifyWithDependencies(
		                 nullptr, {}, true, dependencies) == PAM_SYSTEM_ERR,
		             "null PAM handle maps username lookup failure to PAM_SYSTEM_ERR");

		ScopedPamHandle empty_user_handle;
		ok &= Expect(empty_user_handle.Start(&conversation, "") == PAM_SUCCESS,
		             "guard test starts empty-user PAM handle");
		if (empty_user_handle.Get() == nullptr) {
			return false;
		}
		ok &= Expect(howdy::pam::auth_flow::IdentifyWithDependencies(
		                 empty_user_handle.Get(), {}, true, dependencies) == PAM_USER_UNKNOWN,
		             "empty PAM username maps to PAM_USER_UNKNOWN");
		return ok;
	}

	auto ExpectStatusHelpers() -> bool {
		using howdy::pam::auth_flow::ConversationFn;
		using howdy::pam::auth_flow::HowdyError;
		using howdy::pam::auth_flow::HowdyStatus;

		bool                 ok            = true;
		int                  calls         = 0;
		int                  last_msg_type = 0;
		std::string          last_message;
		const ConversationFn conversation =
		    [&](const howdy::pam::ConversationMessage &message) -> int {
			++calls;
			last_msg_type = message.style;
			last_message  = message.text;
			return PAM_SUCCESS;
		};

		const auto make_status = [](howdy::native::CompareExit exit_code) -> int {
			return howdy::native::EncodeWaitExitStatus(exit_code);
		};

		ok &= Expect(HowdyError(nullptr, make_status(howdy::native::CompareExit::kTimeoutReached),
		                        conversation) == PAM_AUTH_ERR,
		             "timeout status fails closed");
		ok &= Expect(calls == 1 && last_msg_type == PAM_ERROR_MSG &&
		                 last_message == "Face verification timed out",
		             "timeout status sends error conversation");

		calls = 0;
		ok &= Expect(HowdyError(nullptr, make_status(howdy::native::CompareExit::kNoFaceModel),
		                        conversation) == PAM_AUTH_ERR,
		             "no-model status fails closed");
		ok &= Expect(calls == 0, "no-model status sends no conversation");
		ok &= Expect(HowdyError(nullptr, SIGTERM, conversation) == PAM_AUTH_ERR,
		             "signaled status fails closed");
		ok &= Expect(calls == 0, "signaled status sends no conversation");
		ok &= Expect(HowdyError(nullptr, W_STOPCODE(SIGSTOP), conversation) == PAM_AUTH_ERR,
		             "stopped status fails closed");
		ok &= Expect(calls == 0, "stopped status sends no conversation");

		howdy::native::RuntimeConfig confirmation_config;
		confirmation_config.core.no_confirmation = false;

		calls                = 0;
		std::string username = "alice";
		ok &= Expect(HowdyStatus(nullptr, username.data(), EXIT_SUCCESS, confirmation_config,
		                         conversation) == PAM_SUCCESS,
		             "successful status approves login");
		ok &= Expect(calls == 1 && last_msg_type == PAM_TEXT_INFO &&
		                 last_message == "Face matched user alice",
		             "successful status sends enabled confirmation");

		howdy::native::RuntimeConfig quiet_config;
		quiet_config.core.no_confirmation = true;

		calls = 0;
		ok &= Expect(HowdyStatus(nullptr, username.data(), EXIT_SUCCESS, quiet_config,
		                         conversation) == PAM_SUCCESS,
		             "quiet successful status approves login");
		ok &= Expect(calls == 0, "quiet successful status sends no confirmation");

		ok &= Expect(HowdyStatus(nullptr, username.data(),
		                         make_status(howdy::native::CompareExit::kTooDark), quiet_config,
		                         conversation) == PAM_AUTH_ERR,
		             "failed status delegates to error handling");
		ok &= Expect(calls == 1 && last_msg_type == PAM_ERROR_MSG &&
		                 last_message == "Camera image is too dark for detection",
		             "failed status sends mapped error conversation");

		return ok;
	}

}  // namespace

auto main() -> int {
	bool ok = true;

	ok &= ExpectConversationHelpers();
	ok &= ExpectStatusHelpers();
	ok &= ExpectAuthenticationGuards();
	ok &= RunAuthFlowIntegrationTests();

	return ok ? 0 : 1;
}
