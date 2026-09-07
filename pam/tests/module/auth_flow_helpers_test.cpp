#include "module/auth_flow_test_support.hpp"
#include "protocol/auth_helper_protocol.hpp"
#include "protocol/compare_exit.hpp"
#include "test_support.hpp"

#include <csignal>

#include <sys/wait.h>

namespace {
	using namespace howdy::test::auth_flow;

	using howdy::test::expect;

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
		SendConversationMessage(direct_conversation, PAM_ERROR_MSG, "direct message");
		ok &= expect(direct_calls == 1 && direct_type == PAM_ERROR_MSG &&
		                 direct_message == "direct message",
		             "message helper invokes conversation despite conversation failure");
		direct_result = PAM_SUCCESS;
		SendConversationMessage(direct_conversation, PAM_TEXT_INFO, "successful message");
		ok &= expect(direct_calls == 2 && direct_type == PAM_TEXT_INFO &&
		                 direct_message == "successful message",
		             "message helper invokes successful conversation");

		ConversationState state;
		struct pam_conv   conversation{
		    .conv        = TestConversation,
		    .appdata_ptr = &state,
		};
		ScopedPamHandle pam_handle;
		ok &= expect(pam_handle.Start(&conversation) == PAM_SUCCESS, "starts PAM handle");
		if (pam_handle.Get() == nullptr) {
			return false;
		}

		ok &= expect(!AuthTokenPresent(pam_handle.Get()), "missing auth token is absent");

		return ok;
	}

	auto ExpectAuthenticationGuards() -> bool {
		EligibilityFlowFixture fixture;
		auto                   dependencies = MakeEligibilityFlowDependencies(&fixture);
		ConversationState      state;
		struct pam_conv        conversation{
		    .conv        = TestConversation,
		    .appdata_ptr = &state,
		};
		bool ok = true;

		ScopedPamHandle valid_handle;
		ok &= expect(valid_handle.Start(&conversation) == PAM_SUCCESS,
		             "guard test starts valid PAM handle");
		if (valid_handle.Get() == nullptr) {
			return false;
		}
		ok &= expect(howdy::pam::auth_flow::IdentifyWithDependencies(
		                 nullptr, {}, true, dependencies) == PAM_SYSTEM_ERR,
		             "null PAM handle maps username lookup failure to PAM_SYSTEM_ERR");

		ScopedPamHandle empty_user_handle;
		ok &= expect(empty_user_handle.Start(&conversation, "") == PAM_SUCCESS,
		             "guard test starts empty-user PAM handle");
		if (empty_user_handle.Get() == nullptr) {
			return false;
		}
		ok &= expect(howdy::pam::auth_flow::IdentifyWithDependencies(
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
			return static_cast<int>(exit_code) << 8;
		};

		ok &= expect(HowdyError(make_status(howdy::native::CompareExit::kTimeoutReached),
		                        conversation) == PAM_AUTH_ERR,
		             "timeout status fails closed");
		ok &= expect(calls == 1 && last_msg_type == PAM_ERROR_MSG &&
		                 last_message == "Face verification timed out",
		             "timeout status sends error conversation");

		calls = 0;
		ok &= expect(HowdyError(make_status(howdy::native::CompareExit::kNoFaceModel),
		                        conversation) == PAM_AUTH_ERR,
		             "no-model status fails closed");
		ok &= expect(calls == 0, "no-model status sends no conversation");
		ok &= expect(HowdyError(SIGTERM, conversation) == PAM_AUTH_ERR,
		             "signaled status fails closed");
		ok &= expect(calls == 0, "signaled status sends no conversation");
		ok &= expect(HowdyError(W_STOPCODE(SIGSTOP), conversation) == PAM_AUTH_ERR,
		             "stopped status fails closed");
		ok &= expect(calls == 0, "stopped status sends no conversation");

		howdy::native::RuntimeConfig confirmation_config;
		confirmation_config.core.no_confirmation = false;

		calls                = 0;
		std::string username = "alice";
		ok &= expect(HowdyStatus(username.data(), EXIT_SUCCESS, confirmation_config,
		                         conversation) == PAM_SUCCESS,
		             "successful status approves login");
		ok &= expect(calls == 1 && last_msg_type == PAM_TEXT_INFO &&
		                 last_message == "Face matched user alice",
		             "successful status sends enabled confirmation");

		howdy::native::RuntimeConfig quiet_config;
		quiet_config.core.no_confirmation = true;

		calls = 0;
		ok &= expect(HowdyStatus(username.data(), EXIT_SUCCESS, quiet_config, conversation) ==
		                 PAM_SUCCESS,
		             "quiet successful status approves login");
		ok &= expect(calls == 0, "quiet successful status sends no confirmation");

		ok &= expect(HowdyStatus(username.data(), make_status(howdy::native::CompareExit::kTooDark),
		                         quiet_config, conversation) == PAM_AUTH_ERR,
		             "failed status delegates to error handling");
		ok &= expect(calls == 1 && last_msg_type == PAM_ERROR_MSG &&
		                 last_message == "Camera image is too dark for detection",
		             "failed status sends mapped error conversation");

		return ok;
	}

}  // namespace

auto RunAuthHelperFdTests() -> bool;
auto RunProcessWaitTests() -> bool;
auto RunAuthHelperOutputTests() -> bool;
auto RunAuthFlowIntegrationTests() -> bool;

auto main() -> int {
	using namespace howdy::native::auth_helper_protocol;

	bool ok = true;

	ok &= RunAuthHelperFdTests();
	ok &= RunProcessWaitTests();
	ok &= RunAuthHelperOutputTests();
	ok &= ExpectConversationHelpers();
	ok &= ExpectStatusHelpers();
	ok &= ExpectAuthenticationGuards();
	ok &= RunAuthFlowIntegrationTests();

	ok &= expect(std::string(kConfigPathKey) == "CONFIG_PATH",
	             "config path protocol key remains unchanged");
	ok &= expect(std::string(kUserModelsDirKey) == "USER_MODELS_DIR",
	             "user models directory protocol key remains unchanged");

	return ok ? 0 : 1;
}
