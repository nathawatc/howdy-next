#include "module/entrypoint.hpp"

#include "runtime/message_locale.hpp"

#include <exception>
#include <syslog.h>

namespace howdy::pam {

	auto run_authentication_entrypoint(pam_handle_t *pamh, PamModuleArguments arguments,
	                                   bool                          request_auth_token,
	                                   const EntrypointDependencies &dependencies) noexcept -> int {
		ScopedMessageLocale message_locale;
		try {
			if (dependencies.authenticate == nullptr) {
				return PAM_SYSTEM_ERR;
			}
			return dependencies.authenticate(dependencies.context, pamh, arguments,
			                                 request_auth_token);
		} catch (const std::exception &error) {
			syslog(LOG_ERR, "Unhandled C++ exception in pam_sm_authenticate: %s", error.what());
			return PAM_SYSTEM_ERR;
		} catch (...) {
			syslog(LOG_ERR, "Unhandled non-standard exception in pam_sm_authenticate");
			return PAM_SYSTEM_ERR;
		}
	}

	auto run_pam_authenticate(pam_handle_t *pamh, int flags, int argc, const char **argv,
	                          const EntrypointDependencies &dependencies) noexcept -> int {
		return run_authentication_entrypoint(pamh,
		                                     {
		                                         .flags = flags,
		                                         .argc  = argc,
		                                         .argv  = argv,
		                                     },
		                                     true, dependencies);
	}

}  // namespace howdy::pam
