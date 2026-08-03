#include "module/production_entrypoint.hpp"

#include "module/auth_flow.hpp"

namespace {

	auto production_authenticate(void *context, pam_handle_t *pamh,
	                             howdy::pam::PamModuleArguments arguments, bool request_auth_token)
	    -> int {
		(void)context;
		return howdy::pam::auth_flow::identify_with_dependencies(
		    pamh, arguments, request_auth_token,
		    howdy::pam::auth_flow::production_identify_dependencies());
	}

}  // namespace

namespace howdy::pam {

	auto production_entrypoint_dependencies() noexcept -> EntrypointDependencies {
		return {
		    .context      = nullptr,
		    .authenticate = production_authenticate,
		};
	}

}  // namespace howdy::pam
