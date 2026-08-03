#include "module/entrypoint.hpp"
#include "test_support.hpp"

#include <array>
#include <clocale>
#include <cstdlib>
#include <libintl.h>
#include <stdexcept>
#include <string>

#include <security/pam_modules.h>

namespace {

	using howdy::pam::EntrypointDependencies;
	using howdy::pam::PamModuleArguments;
	using howdy::pam::run_authentication_entrypoint;
	using howdy::test::expect;

	struct AuthCall {
		void              *expected_context = nullptr;
		pam_handle_t      *expected_pamh    = nullptr;
		PamModuleArguments expected_args{};
		bool               expected_request_auth_token = false;
		int                result                      = PAM_SUCCESS;
		int                throw_mode                  = 0;
		int                calls                       = 0;
		void              *actual_context              = nullptr;
		pam_handle_t      *actual_pamh                 = nullptr;
		PamModuleArguments actual_args{};
		bool               actual_request_auth_token = false;
	};

	class ScopedEnvironment {
	public:
		explicit ScopedEnvironment(const char *name)
		    : name_(name) {
			const char *value = std::getenv(name_.c_str());
			if (value != nullptr) {
				had_original_ = true;
				original_     = value;
			}
		}

		ScopedEnvironment(const ScopedEnvironment &)                     = delete;
		auto operator=(const ScopedEnvironment &) -> ScopedEnvironment & = delete;

		~ScopedEnvironment() {
			if (had_original_) {
				setenv(name_.c_str(), original_.c_str(), 1);
			} else {
				unsetenv(name_.c_str());
			}
		}

		void set(const char *value) {
			setenv(name_.c_str(), value, 1);
		}

	private:
		std::string name_;
		std::string original_;
		bool        had_original_ = false;
	};

	auto fake_authenticate(void *context, pam_handle_t *pamh, PamModuleArguments arguments,
	                       bool request_auth_token) -> int {
		auto &call = *static_cast<AuthCall *>(context);
		++call.calls;
		call.actual_context            = context;
		call.actual_pamh               = pamh;
		call.actual_args               = arguments;
		call.actual_request_auth_token = request_auth_token;
		if (call.throw_mode == 1) {
			throw std::runtime_error("simulated entrypoint failure");
		}
		if (call.throw_mode == 2) {
			throw 1;
		}
		return call.result;
	}

	auto dependencies_for(AuthCall &call) -> EntrypointDependencies {
		return {
		    .context      = &call,
		    .authenticate = fake_authenticate,
		};
	}

	auto expect_locale_restored(AuthCall &call, int expected_result) -> bool {
		const locale_t    host_locale = uselocale(nullptr);
		const char       *domain_ptr  = textdomain(nullptr);
		const std::string host_domain =
		    domain_ptr == nullptr ? std::string{} : std::string(domain_ptr);
		bool escaped = false;
		int  result  = PAM_SUCCESS;
		try {
			result = run_authentication_entrypoint(nullptr, {}, true, dependencies_for(call));
		} catch (...) {
			escaped = true;
		}
		return expect(result == expected_result, "entrypoint returns expected callback status") &&
		       expect(!escaped, "entrypoint does not throw") &&
		       expect(uselocale(nullptr) == host_locale,
		              "entrypoint restores host thread locale") &&
		       expect(std::string(textdomain(nullptr)) == host_domain,
		              "entrypoint preserves host gettext domain");
	}

}  // namespace

auto main() -> int {
	bool ok = true;

	int                         marker   = 0;
	std::array<const char *, 3> raw_argv = {"unrelated", nullptr, "workaround=input"};
	AuthCall                    call{
	    .expected_context            = nullptr,
	    .expected_pamh               = reinterpret_cast<pam_handle_t *>(&marker),
	    .expected_args               = {.flags = 41,
	                                    .argc  = static_cast<int>(raw_argv.size()),
	                                    .argv  = raw_argv.data()},
	    .expected_request_auth_token = true,
	    .result                      = PAM_AUTH_ERR,
	};
	call.expected_context = &call;
	const int callback_result =
	    run_authentication_entrypoint(call.expected_pamh, call.expected_args,
	                                  call.expected_request_auth_token, dependencies_for(call));
	ok &= expect(callback_result == PAM_AUTH_ERR, "entrypoint propagates callback result");
	ok &= expect(call.calls == 1, "entrypoint invokes callback exactly once");
	ok &= expect(call.actual_context == call.expected_context, "entrypoint forwards context");
	ok &= expect(call.actual_pamh == call.expected_pamh, "entrypoint forwards PAM handle");
	ok &= expect(call.actual_args.flags == call.expected_args.flags &&
	                 call.actual_args.argc == call.expected_args.argc &&
	                 call.actual_args.argv == call.expected_args.argv,
	             "entrypoint forwards flags, argc, and argv");
	ok &= expect(call.actual_request_auth_token == call.expected_request_auth_token,
	             "entrypoint forwards auth-token request flag");

	call        = {};
	call.result = PAM_USER_UNKNOWN;
	ok &= expect_locale_restored(call, PAM_USER_UNKNOWN);
	ok &= expect(call.calls == 1, "failure callback still runs exactly once");

	call            = {};
	call.throw_mode = 1;
	ok &= expect_locale_restored(call, PAM_SYSTEM_ERR);
	ok &= expect(call.calls == 1, "std exception callback runs exactly once");

	call            = {};
	call.throw_mode = 2;
	ok &= expect_locale_restored(call, PAM_SYSTEM_ERR);
	ok &= expect(call.calls == 1, "unknown exception callback runs exactly once");

	const locale_t null_callback_locale = uselocale(nullptr);
	const int      null_callback_result = run_authentication_entrypoint(
	    nullptr, {}, true, {.context = nullptr, .authenticate = nullptr});
	ok &=
	    expect(null_callback_result == PAM_SYSTEM_ERR, "null authentication callback fails closed");
	ok &= expect(uselocale(nullptr) == null_callback_locale,
	             "null authentication callback restores host thread locale");

	{
		ScopedEnvironment invalid_locale("LC_ALL");
		invalid_locale.set("howdy-invalid-locale");
		call = {};
		ok &= expect(run_authentication_entrypoint(nullptr, {}, true, dependencies_for(call)) ==
		                 PAM_SUCCESS,
		             "locale setup failure does not fail authentication");
		ok &= expect(call.calls == 1, "locale setup failure still invokes authentication once");
	}

	const std::string global_locale = []() -> std::string {
		const char *locale = std::setlocale(LC_ALL, nullptr);
		return locale == nullptr ? std::string{} : std::string(locale);
	}();
	const std::string text_domain = []() -> std::string {
		const char *domain = textdomain(nullptr);
		return domain == nullptr ? std::string{} : std::string(domain);
	}();
	call = {};
	ok &= expect(run_authentication_entrypoint(nullptr, {}, false, dependencies_for(call)) ==
	                 PAM_SUCCESS,
	             "entrypoint forwards false auth-token request");
	ok &= expect(std::string(std::setlocale(LC_ALL, nullptr)) == global_locale,
	             "entrypoint leaves process-global locale unchanged");
	ok &= expect(std::string(textdomain(nullptr)) == text_domain,
	             "entrypoint leaves host gettext domain unchanged");

	return ok ? 0 : 1;
}
