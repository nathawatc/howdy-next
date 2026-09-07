#include "module/production_entrypoint.hpp"

#include "module/auth_flow.hpp"

namespace {

	auto ProductionAuthenticate(void *context, pam_handle_t *pamh,
	                            howdy::pam::PamModuleArguments arguments, bool request_auth_token)
	    -> int {
		(void)context;
		return howdy::pam::auth_flow::IdentifyWithDependencies(
		    pamh, arguments, request_auth_token,
		    howdy::pam::auth_flow::ProductionIdentifyDependencies());
	}

}  // namespace

namespace howdy::pam {

	auto ProductionEntrypointDependencies() noexcept -> EntrypointDependencies {
		return {
		    .context      = nullptr,
		    .authenticate = ProductionAuthenticate,
		};
	}

}  // namespace howdy::pam
