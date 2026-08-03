#pragma once

#include "runtime/environment.hpp"

#include <security/pam_appl.h>

namespace howdy::pam::runtime {

	struct SessionState {
		bool ssh_session = false;
	};

	auto probe_session_state(pam_handle_t *pamh, const EnvironmentLookupDependencies &dependencies)
	    -> SessionState;

	auto production_ssh_session_present(void *context, pam_handle_t *pamh) -> bool;

}  // namespace howdy::pam::runtime
