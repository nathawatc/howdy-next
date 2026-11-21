#include "runtime/session_probe.hpp"

#include "runtime/environment.hpp"

#include <array>
#include <string_view>

namespace howdy::pam::runtime {

	namespace {

		// A convenience check, not a security boundary: it reads only these markers in the PAM
		// and process environment, and the caller controls both. An SSH user can run
		// `env -u SSH_CONNECTION -u SSH_CLIENT -u SSH_TTY sudo ...` and get a face check on the
		// local camera. This is intentionally unfixed. A logind (sd-login) approach was tried and
		// dropped: many legitimate callers run outside any logind session (GUI terminals, the gdm
		// worker, the socket-activated polkit helper), and so does `systemd-run --user`, so no
		// logind signal is exact. Every heuristic either left a bypass (`su` to another user,
		// lingering processes of an ended SSH session, sshd/xrdp stacks before a session exists,
		// tmux started locally) or disabled local face unlock while any SSH session was open. Do
		// not reopen this without a process-ancestry design that addresses all of those cases.
		constexpr std::array<std::string_view, 4> kSshEnvironmentMarkers = {
		    "SSH_CONNECTION",
		    "SSH_CLIENT",
		    "SSH_TTY",
		    "SSHD_OPTS",
		};

	}  // namespace

	auto ProbeSessionState(pam_handle_t *pamh, const EnvironmentLookupDependencies &dependencies)
	    -> SessionState {
		for (const std::string_view marker : kSshEnvironmentMarkers) {
			if (FindEnvironmentVariable(pamh, marker, dependencies) !=
			    EnvironmentSource::kMissing) {
				return {.ssh_session = true};
			}
		}
		return {.ssh_session = false};
	}

	auto ProductionSshSessionPresent(void *context, pam_handle_t *pamh) -> bool {
		(void)context;
		return ProbeSessionState(pamh, ProductionEnvironmentLookupDependencies()).ssh_session;
	}

}  // namespace howdy::pam::runtime
