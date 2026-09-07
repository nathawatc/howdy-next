#pragma once

#include "runtime/environment.hpp"

#include <security/pam_appl.h>

namespace howdy::pam::runtime {

	struct SessionState {
		bool ssh_session = false;
	};

	auto ProbeSessionState(pam_handle_t *pamh, const EnvironmentLookupDependencies &dependencies)
	    -> SessionState;

	auto ProductionSshSessionPresent(void *context, pam_handle_t *pamh) -> bool;

}  // namespace howdy::pam::runtime
