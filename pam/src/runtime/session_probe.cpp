#include "runtime/session_probe.hpp"

#include <array>
#include <string_view>

namespace howdy::pam::runtime {

	namespace {

		constexpr std::array<std::string_view, 4> kSshEnvironmentMarkers = {
		    "SSH_CONNECTION",
		    "SSH_CLIENT",
		    "SSH_TTY",
		    "SSHD_OPTS",
		};

	}  // namespace

	auto probe_session_state(pam_handle_t *pamh, const EnvironmentLookupDependencies &dependencies)
	    -> SessionState {
		for (const std::string_view marker : kSshEnvironmentMarkers) {
			if (find_environment_variable(pamh, marker, dependencies) !=
			    EnvironmentSource::kMissing) {
				return {.ssh_session = true};
			}
		}
		return {.ssh_session = false};
	}

	auto production_ssh_session_present(void *context, pam_handle_t *pamh) -> bool {
		(void)context;
		return probe_session_state(pamh, production_environment_lookup_dependencies()).ssh_session;
	}

}  // namespace howdy::pam::runtime
