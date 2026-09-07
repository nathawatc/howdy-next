#include "module/entrypoint.hpp"
#include "module/production_entrypoint.hpp"

// Authentication is the only supported PAM service-module operation.
PAM_EXTERN auto pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc, const char **argv)
    -> int {
	return howdy::pam::RunPamAuthenticate(pamh, flags, argc, argv,
	                                      howdy::pam::ProductionEntrypointDependencies());
}
