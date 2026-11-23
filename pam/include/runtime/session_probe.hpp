#pragma once

#include "runtime/environment.hpp"

#include <security/pam_appl.h>

namespace howdy::pam::runtime {

	struct SessionState {
		bool ssh_session = false;
	};

	[[nodiscard]] auto ProbeSessionState(pam_handle_t                        *pamh,
	                                     const EnvironmentLookupDependencies &dependencies)
	    -> SessionState;

	[[nodiscard]] auto ProductionSshSessionPresent(void *context, pam_handle_t *pamh) -> bool;

}  // namespace howdy::pam::runtime
