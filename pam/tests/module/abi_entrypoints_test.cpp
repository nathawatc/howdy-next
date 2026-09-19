#include "module/entrypoint.hpp"
#include "module/pam_options.hpp"
#include "pam_test_support.hpp"
#include "test_support.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <stdexcept>

#include <security/pam_modules.h>

namespace {

	using howdy::pam::EntrypointDependencies;
	using howdy::pam::PamModuleArguments;
	using howdy::pam::RunPamAuthenticate;
	using howdy::test::Expect;

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
			howdy::test::ThrowNonStandardExceptionForTest();
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

	std::array<const char *, 3> argv = {"workaround=input", nullptr, "after"};
	auto *const                 pamh = howdy::test::OpaquePamHandleForTest();
	CallbackState               state{.result = PAM_AUTH_ERR};

	const int result = RunPamAuthenticate(pamh, 73, static_cast<int>(argv.size()), argv.data(),
	                                      DependenciesFor(state));
	ok &= Expect(result == PAM_AUTH_ERR, "ABI adapter propagates callback result");
	ok &= Expect(state.calls == 1, "ABI adapter invokes callback exactly once");
	ok &= Expect(state.context == &state, "ABI adapter forwards dependency context");
	ok &= Expect(state.pamh == pamh, "ABI adapter forwards PAM handle");
	ok &= Expect(state.arguments.flags == 73, "ABI adapter forwards flags");
	ok &= Expect(state.arguments.argc == 3, "ABI adapter forwards argc");
	ok &= Expect(state.arguments.argv == argv.data(), "ABI adapter forwards argv pointer");
	bool forwarded_entries_match = false;
	if (state.arguments.argv != nullptr && state.arguments.argc == 3) {
		const auto forwarded    = std::span(state.arguments.argv, std::size_t{3});
		forwarded_entries_match = forwarded.front() == argv.front() &&
		                          forwarded.subspan(1).front() == nullptr &&
		                          forwarded.back() == argv.back();
	}
	ok &= Expect(forwarded_entries_match, "ABI adapter forwards argv entries");
	ok &= Expect(state.request_auth_token, "ABI adapter always requests auth token");

	state                 = {};
	state.result          = PAM_USER_UNKNOWN;
	bool escaped          = false;
	int  exception_result = PAM_SUCCESS;
	try {
		exception_result = RunPamAuthenticate(pamh, 0, 0, nullptr, DependenciesFor(state));
	} catch (...) {
		escaped = true;
	}
	ok &= Expect(exception_result == PAM_USER_UNKNOWN, "ABI adapter preserves callback status");
	ok &= Expect(state.calls == 1, "ABI adapter invokes second callback exactly once");
	ok &= Expect(!escaped, "ABI adapter does not throw for normal callback");

	state            = {};
	state.throw_mode = 1;
	escaped          = false;
	int std_result   = PAM_SUCCESS;
	try {
		std_result = RunPamAuthenticate(pamh, 0, 0, nullptr, DependenciesFor(state));
	} catch (...) {
		escaped = true;
	}
	ok &= Expect(std_result == PAM_SYSTEM_ERR, "ABI adapter maps std exception to PAM_SYSTEM_ERR");
	ok &= Expect(state.calls == 1, "std exception callback runs once");
	ok &= Expect(!escaped, "std exception does not cross ABI adapter");

	state              = {};
	state.throw_mode   = 2;
	escaped            = false;
	int unknown_result = PAM_SUCCESS;
	try {
		unknown_result = RunPamAuthenticate(pamh, 0, 0, nullptr, DependenciesFor(state));
	} catch (...) {
		escaped = true;
	}
	ok &= Expect(unknown_result == PAM_SYSTEM_ERR,
	             "ABI adapter maps unknown exception to PAM_SYSTEM_ERR");
	ok &= Expect(state.calls == 1, "unknown exception callback runs once");
	ok &= Expect(!escaped, "unknown exception does not cross ABI adapter");

	ok &=
	    Expect(RunPamAuthenticate(pamh, 0, 0, nullptr,
	                              {.context = nullptr, .authenticate = nullptr}) == PAM_SYSTEM_ERR,
	           "ABI adapter rejects null callback");

	ok &= Expect(pam_sm_open_session(nullptr, 0, 0, nullptr) == PAM_IGNORE,
	             "open_session remains ignored");
	ok &=
	    Expect(pam_sm_acct_mgmt(nullptr, 0, 0, nullptr) == PAM_IGNORE, "acct_mgmt remains ignored");
	ok &= Expect(pam_sm_close_session(nullptr, 0, 0, nullptr) == PAM_IGNORE,
	             "close_session remains ignored");
	ok &=
	    Expect(pam_sm_chauthtok(nullptr, 0, 0, nullptr) == PAM_IGNORE, "chauthtok remains ignored");
	ok &= Expect(pam_sm_setcred(nullptr, 0, 0, nullptr) == PAM_IGNORE, "setcred remains ignored");

	return ok ? 0 : 1;
}
