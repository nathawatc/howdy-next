#pragma once

#include "module/pam_options.hpp"
#include "runtime/environment.hpp"

#include <security/pam_modules.h>

namespace howdy::pam {

	using AuthenticateFn = int (*)(void *context, pam_handle_t *pamh, PamModuleArguments arguments,
	                               bool request_auth_token);

	struct EntrypointDependencies {
		void          *context      = nullptr;
		AuthenticateFn authenticate = nullptr;
		// Where the message locale is read from the PAM environment. The default has no
		// lookup, so tests with an opaque PAM handle never call pam_getenv.
		runtime::EnvironmentLookupDependencies environment{};
	};

	__attribute__((visibility("hidden"))) auto
	RunAuthenticationEntrypoint(pam_handle_t *pamh, PamModuleArguments arguments,
	                            bool                          request_auth_token,
	                            const EntrypointDependencies &dependencies) noexcept -> int;

	__attribute__((visibility("hidden"))) auto
	RunPamAuthenticate(pam_handle_t *pamh, int flags, int argc, const char **argv,
	                   const EntrypointDependencies &dependencies) noexcept -> int;

}  // namespace howdy::pam
