#pragma once

#include "module/pam_options.hpp"

#include <security/pam_modules.h>

namespace howdy::pam {

	using AuthenticateFn = int (*)(void *context, pam_handle_t *pamh, PamModuleArguments arguments,
	                               bool request_auth_token);

	struct EntrypointDependencies {
		void          *context      = nullptr;
		AuthenticateFn authenticate = nullptr;
	};

	__attribute__((visibility("hidden"))) auto
	run_authentication_entrypoint(pam_handle_t *pamh, PamModuleArguments arguments,
	                              bool                          request_auth_token,
	                              const EntrypointDependencies &dependencies) noexcept -> int;

	__attribute__((visibility("hidden"))) auto
	run_pam_authenticate(pam_handle_t *pamh, int flags, int argc, const char **argv,
	                     const EntrypointDependencies &dependencies) noexcept -> int;

}  // namespace howdy::pam
