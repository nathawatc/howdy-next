#include "module/entrypoint.hpp"
#include "module/production_entrypoint.hpp"

// Authentication is the only supported PAM service-module operation.
PAM_EXTERN auto pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc, const char **argv)
    -> int {
	return howdy::pam::run_pam_authenticate(pamh, flags, argc, argv,
	                                        howdy::pam::production_entrypoint_dependencies());
}
