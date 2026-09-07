#include "module/entrypoint.hpp"
#include "test_support.hpp"

#include <array>
#include <stdexcept>

#include <security/pam_modules.h>

namespace {

	using howdy::pam::EntrypointDependencies;
	using howdy::pam::PamModuleArguments;
	using howdy::pam::RunPamAuthenticate;
	using howdy::test::expect;

	struct CallbackState {
		int                result     = PAM_SUCCESS;
		int                throw_mode = 0;
		int                calls      = 0;
		void              *context    = nullptr;
		pam_handle_t      *pamh       = nullptr;
		PamModuleArguments arguments{};
		bool               request_auth_token = false;
	};

	auto FakeAuthenticate(void *context, pam_handle_t *pamh, PamModuleArguments arguments,
	                      bool request_auth_token) -> int {
		auto &state = *static_cast<CallbackState *>(context);
		++state.calls;
		state.context            = context;
		state.pamh               = pamh;
		state.arguments          = arguments;
		state.request_auth_token = request_auth_token;
		if (state.throw_mode == 1) {
			throw std::runtime_error("simulated ABI adapter failure");
		}
		if (state.throw_mode == 2) {
			throw 1;
		}
		return state.result;
	}

	auto DependenciesFor(CallbackState &state) -> EntrypointDependencies {
		return {
		    .context      = &state,
		    .authenticate = FakeAuthenticate,
		};
	}

}  // namespace

auto main() -> int {
	bool ok = true;

	int                         marker = 0;
	std::array<const char *, 3> argv   = {"workaround=input", nullptr, "after"};
	auto *const                 pamh   = reinterpret_cast<pam_handle_t *>(&marker);
	CallbackState               state{.result = PAM_AUTH_ERR};

	const int result = RunPamAuthenticate(pamh, 73, static_cast<int>(argv.size()), argv.data(),
	                                      DependenciesFor(state));
	ok &= expect(result == PAM_AUTH_ERR, "ABI adapter propagates callback result");
	ok &= expect(state.calls == 1, "ABI adapter invokes callback exactly once");
	ok &= expect(state.context == &state, "ABI adapter forwards dependency context");
	ok &= expect(state.pamh == pamh, "ABI adapter forwards PAM handle");
	ok &= expect(state.arguments.flags == 73, "ABI adapter forwards flags");
	ok &= expect(state.arguments.argc == 3, "ABI adapter forwards argc");
	ok &= expect(state.arguments.argv == argv.data(), "ABI adapter forwards argv pointer");
	ok &= expect(state.arguments.argv[0] == argv[0] && state.arguments.argv[1] == nullptr &&
	                 state.arguments.argv[2] == argv[2],
	             "ABI adapter forwards argv entries");
	ok &= expect(state.request_auth_token, "ABI adapter always requests auth token");

	state                 = {};
	state.result          = PAM_USER_UNKNOWN;
	bool escaped          = false;
	int  exception_result = PAM_SUCCESS;
	try {
		exception_result = RunPamAuthenticate(pamh, 0, 0, nullptr, DependenciesFor(state));
	} catch (...) {
		escaped = true;
	}
	ok &= expect(exception_result == PAM_USER_UNKNOWN, "ABI adapter preserves callback status");
	ok &= expect(state.calls == 1, "ABI adapter invokes second callback exactly once");
	ok &= expect(!escaped, "ABI adapter does not throw for normal callback");

	state            = {};
	state.throw_mode = 1;
	escaped          = false;
	int std_result   = PAM_SUCCESS;
	try {
		std_result = RunPamAuthenticate(pamh, 0, 0, nullptr, DependenciesFor(state));
	} catch (...) {
		escaped = true;
	}
	ok &= expect(std_result == PAM_SYSTEM_ERR, "ABI adapter maps std exception to PAM_SYSTEM_ERR");
	ok &= expect(state.calls == 1, "std exception callback runs once");
	ok &= expect(!escaped, "std exception does not cross ABI adapter");

	state              = {};
	state.throw_mode   = 2;
	escaped            = false;
	int unknown_result = PAM_SUCCESS;
	try {
		unknown_result = RunPamAuthenticate(pamh, 0, 0, nullptr, DependenciesFor(state));
	} catch (...) {
		escaped = true;
	}
	ok &= expect(unknown_result == PAM_SYSTEM_ERR,
	             "ABI adapter maps unknown exception to PAM_SYSTEM_ERR");
	ok &= expect(state.calls == 1, "unknown exception callback runs once");
	ok &= expect(!escaped, "unknown exception does not cross ABI adapter");

	ok &=
	    expect(RunPamAuthenticate(pamh, 0, 0, nullptr,
	                              {.context = nullptr, .authenticate = nullptr}) == PAM_SYSTEM_ERR,
	           "ABI adapter rejects null callback");

	ok &= expect(pam_sm_open_session(nullptr, 0, 0, nullptr) == PAM_IGNORE,
	             "open_session remains ignored");
	ok &=
	    expect(pam_sm_acct_mgmt(nullptr, 0, 0, nullptr) == PAM_IGNORE, "acct_mgmt remains ignored");
	ok &= expect(pam_sm_close_session(nullptr, 0, 0, nullptr) == PAM_IGNORE,
	             "close_session remains ignored");
	ok &=
	    expect(pam_sm_chauthtok(nullptr, 0, 0, nullptr) == PAM_IGNORE, "chauthtok remains ignored");
	ok &= expect(pam_sm_setcred(nullptr, 0, 0, nullptr) == PAM_IGNORE, "setcred remains ignored");

	return ok ? 0 : 1;
}
