#include "module/entrypoint.hpp"
#include "module/pam_options.hpp"
#include "pam_test_support.hpp"
#include "test_support.hpp"

#include <array>
#include <clocale>
#include <stdexcept>
#include <string>
#include <string_view>

#include <langinfo.h>
#include <libintl.h>
#include <locale.h>
#include <stdlib.h>

namespace {

	using howdy::pam::EntrypointDependencies;
	using howdy::pam::PamModuleArguments;
	using howdy::pam::RunAuthenticationEntrypoint;
	using howdy::test::Expect;
	using ScopedEnvironment = howdy::test::ScopedEnvironmentVariable;

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

	auto FakeAuthenticate(void *context, pam_handle_t *pamh, PamModuleArguments arguments,
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
			howdy::test::ThrowNonStandardExceptionForTest();
		}
		return call.result;
	}

	auto DependenciesFor(AuthCall &call) -> EntrypointDependencies {
		return {
		    .context      = &call,
		    .authenticate = FakeAuthenticate,
		};
	}

	auto ExpectLocaleRestored(AuthCall &call, int expected_result) -> bool {
		const locale_t    host_locale = uselocale(nullptr);
		const char       *domain_ptr  = textdomain(nullptr);
		const std::string host_domain =
		    domain_ptr == nullptr ? std::string{} : std::string(domain_ptr);
		bool escaped = false;
		int  result  = PAM_SUCCESS;
		try {
			result = RunAuthenticationEntrypoint(nullptr, {}, true, DependenciesFor(call));
		} catch (...) {
			escaped = true;
		}
		return Expect(result == expected_result, "entrypoint returns expected callback status") &&
		       Expect(!escaped, "entrypoint does not throw") &&
		       Expect(uselocale(nullptr) == host_locale,
		              "entrypoint restores host thread locale") &&
		       Expect(std::string(textdomain(nullptr)) == host_domain,
		              "entrypoint preserves host gettext domain");
	}

	// Records what the entrypoint hands to the PAM environment lookup and which
	// message locale is active while the authentication callback runs.
	struct PamLocaleProbe {
		pam_handle_t *lookup_pamh = nullptr;
		int           lookups     = 0;
		std::string   active_messages_locale;
	};

	auto ProbeLookup(void *context, pam_handle_t *pamh, const char *name) -> const char * {
		auto &probe       = *static_cast<PamLocaleProbe *>(context);
		probe.lookup_pamh = pamh;
		++probe.lookups;
		return std::string_view(name) == "LANG" ? "C.UTF-8" : nullptr;
	}

	auto ProbeAuthenticate(void *context, pam_handle_t *pamh, PamModuleArguments arguments,
	                       bool request_auth_token) -> int {
		(void)pamh;
		(void)arguments;
		(void)request_auth_token;
		auto &probe = *static_cast<PamLocaleProbe *>(context);
		probe.active_messages_locale =
		    nl_langinfo_l(_NL_LOCALE_NAME(LC_MESSAGES), uselocale(nullptr));
		return PAM_SUCCESS;
	}

	auto ExpectPamEnvironmentReachesLocale() -> bool {
		locale_t utf8_locale = newlocale(LC_MESSAGES_MASK, "C.UTF-8", nullptr);
		if (utf8_locale == nullptr) {
			return howdy::test::SkipUnavailable("locale-c-utf8", "C.UTF-8 locale is not available");
		}
		const std::string utf8_name = nl_langinfo_l(_NL_LOCALE_NAME(LC_MESSAGES), utf8_locale);
		freelocale(utf8_locale);

		const ScopedEnvironment lc_all("LC_ALL");
		const ScopedEnvironment lc_messages("LC_MESSAGES");
		const ScopedEnvironment lang("LANG");

		PamLocaleProbe probe;
		pam_handle_t  *pamh   = howdy::test::OpaquePamHandleForTest();
		const int      result = RunAuthenticationEntrypoint(
		    pamh, {}, true,
		    {
		        .context      = &probe,
		        .authenticate = ProbeAuthenticate,
		        .environment  = {.context = &probe, .pam_environment = ProbeLookup},
		    });
		return Expect(result == PAM_SUCCESS, "probe authentication succeeds") &&
		       Expect(probe.lookups == 3, "entrypoint consults the PAM environment") &&
		       Expect(probe.lookup_pamh == pamh,
		              "entrypoint passes its PAM handle to the lookup") &&
		       Expect(probe.active_messages_locale == utf8_name,
		              "PAM LANG is the message locale during authentication");
	}

}  // namespace

auto main() -> int {
	bool ok = true;

	std::array<const char *, 3> raw_argv = {"unrelated", nullptr, "workaround=input"};
	AuthCall                    call{
	    .expected_context = nullptr,
	    .expected_pamh    = howdy::test::OpaquePamHandleForTest(),
	    .expected_args =
	        {
	            .flags = 41,
	            .argc  = static_cast<int>(raw_argv.size()),
	            .argv  = raw_argv.data(),
	        },
	    .expected_request_auth_token = true,
	    .result                      = PAM_AUTH_ERR,
	};
	call.expected_context = &call;
	const int callback_result =
	    RunAuthenticationEntrypoint(call.expected_pamh, call.expected_args,
		                            call.expected_request_auth_token, DependenciesFor(call));
	ok &= Expect(callback_result == PAM_AUTH_ERR, "entrypoint propagates callback result");
	ok &= Expect(call.calls == 1, "entrypoint invokes callback exactly once");
	ok &= Expect(call.actual_context == call.expected_context, "entrypoint forwards context");
	ok &= Expect(call.actual_pamh == call.expected_pamh, "entrypoint forwards PAM handle");
	ok &= Expect(call.actual_args.flags == call.expected_args.flags &&
	                 call.actual_args.argc == call.expected_args.argc &&
	                 call.actual_args.argv == call.expected_args.argv,
	             "entrypoint forwards flags, argc, and argv");
	ok &= Expect(call.actual_request_auth_token == call.expected_request_auth_token,
	             "entrypoint forwards auth-token request flag");

	call        = {};
	call.result = PAM_USER_UNKNOWN;
	ok &= ExpectLocaleRestored(call, PAM_USER_UNKNOWN);
	ok &= Expect(call.calls == 1, "failure callback still runs exactly once");

	call            = {};
	call.throw_mode = 1;
	ok &= ExpectLocaleRestored(call, PAM_SYSTEM_ERR);
	ok &= Expect(call.calls == 1, "std exception callback runs exactly once");

	call            = {};
	call.throw_mode = 2;
	ok &= ExpectLocaleRestored(call, PAM_SYSTEM_ERR);
	ok &= Expect(call.calls == 1, "unknown exception callback runs exactly once");

	const locale_t null_callback_locale = uselocale(nullptr);
	const int      null_callback_result = RunAuthenticationEntrypoint(
	    nullptr, {}, true, {.context = nullptr, .authenticate = nullptr});
	ok &=
	    Expect(null_callback_result == PAM_SYSTEM_ERR, "null authentication callback fails closed");
	ok &= Expect(uselocale(nullptr) == null_callback_locale,
	             "null authentication callback restores host thread locale");

	{
		ScopedEnvironment invalid_locale("LC_ALL");
		invalid_locale.Set("howdy-invalid-locale");
		call = {};
		ok &= Expect(RunAuthenticationEntrypoint(nullptr, {}, true, DependenciesFor(call)) ==
		                 PAM_SUCCESS,
		             "locale setup failure does not fail authentication");
		ok &= Expect(call.calls == 1, "locale setup failure still invokes authentication once");
	}

	ok &= ExpectPamEnvironmentReachesLocale();

	const std::string global_locale = [] -> std::string {
		const char *locale = std::setlocale(LC_ALL, nullptr);
		return locale == nullptr ? std::string{} : std::string(locale);
	}();
	const std::string text_domain = [] -> std::string {
		const char *domain = textdomain(nullptr);
		return domain == nullptr ? std::string{} : std::string(domain);
	}();
	call = {};
	ok &= Expect(RunAuthenticationEntrypoint(nullptr, {}, false, DependenciesFor(call)) ==
	                 PAM_SUCCESS,
	             "entrypoint forwards false auth-token request");
	ok &= Expect(std::string(std::setlocale(LC_ALL, nullptr)) == global_locale,
	             "entrypoint leaves process-global locale unchanged");
	ok &= Expect(std::string(textdomain(nullptr)) == text_domain,
	             "entrypoint leaves host gettext domain unchanged");

	return ok ? 0 : 1;
}
