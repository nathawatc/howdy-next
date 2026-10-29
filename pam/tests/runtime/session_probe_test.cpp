#include "runtime/environment.hpp"
#include "runtime/environment_test_support.hpp"
#include "runtime/session_probe.hpp"
#include "test_support.hpp"

#include <string>

namespace {

	using howdy::pam::runtime::EnvironmentLookupDependencies;
	using howdy::pam::runtime::ProbeSessionState;
	using howdy::pam::runtime::SessionState;
	using howdy::test::Expect;
	using howdy::test::runtime_environment::Dependencies;
	using howdy::test::runtime_environment::EnvironmentValues;

	auto ExpectSsh(const char *marker, bool pam_source) -> bool {
		EnvironmentValues values;
		if (pam_source) {
			values.pam.emplace(marker, "present");
		} else {
			values.process.emplace(marker, "present");
		}
		const SessionState result = ProbeSessionState(nullptr, Dependencies(&values));
		return result.ssh_session;
	}

}  // namespace

auto main() -> int {
	bool ok = true;

	for (const char *marker : {"SSH_CONNECTION", "SSH_CLIENT", "SSH_TTY", "SSHD_OPTS"}) {
		ok &= Expect(ExpectSsh(marker, true),
		             std::string(marker) + " is detected in PAM environment");
		ok &= Expect(ExpectSsh(marker, false),
		             std::string(marker) + " is detected through process fallback");
	}

	EnvironmentValues values;
	values.pam["PAM_RHOST"]            = "remote-host";
	values.pam["SSH_CONNECTION_EXTRA"] = "prefixed";
	ok &= Expect(!ProbeSessionState(nullptr, Dependencies(&values)).ssh_session,
	             "non-marker variables do not indicate SSH session");

	values.pam.clear();
	values.process.clear();
	values.pam_calls             = 0;
	values.process_calls         = 0;
	values.pam["SSH_CONNECTION"] = "first";
	values.pam["SSH_CLIENT"]     = "second";
	ok &= Expect(ProbeSessionState(nullptr, Dependencies(&values)).ssh_session,
	             "multiple markers are detected");
	ok &= Expect(values.pam_calls == 1 && values.process_calls == 0,
	             "first SSH marker short-circuits later lookups");

	values.pam.clear();
	values.pam["SSH_CLIENT"] = "";
	values.pam["SSH_TTY"]    = "tty";
	ok &= Expect(ProbeSessionState(nullptr, Dependencies(&values)).ssh_session,
	             "empty and multiple marker values are detected");

	const EnvironmentLookupDependencies invalid_dependencies{};
	ok &= Expect(!ProbeSessionState(nullptr, invalid_dependencies).ssh_session,
	             "invalid environment dependencies fail safely");

	return ok ? 0 : 1;
}
