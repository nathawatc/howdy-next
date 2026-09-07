#include "runtime/session_probe.hpp"
#include "test_support.hpp"

#include <map>
#include <string>

namespace {

	using howdy::pam::runtime::EnvironmentLookupDependencies;
	using howdy::pam::runtime::ProbeSessionState;
	using howdy::pam::runtime::SessionState;
	using howdy::test::expect;

	struct EnvironmentValues {
		std::map<std::string, std::string> pam;
		std::map<std::string, std::string> process;
		int                                pam_calls     = 0;
		int                                process_calls = 0;
	};

	auto LookupPam(void *context, pam_handle_t *pamh, const char *name) -> const char * {
		(void)pamh;
		auto *values = static_cast<EnvironmentValues *>(context);
		++values->pam_calls;
		const auto found = values->pam.find(name == nullptr ? "" : name);
		return found == values->pam.end() ? nullptr : found->second.c_str();
	}

	auto LookupProcess(void *context, const char *name) -> const char * {
		auto *values = static_cast<EnvironmentValues *>(context);
		++values->process_calls;
		const auto found = values->process.find(name == nullptr ? "" : name);
		return found == values->process.end() ? nullptr : found->second.c_str();
	}

	auto Dependencies(EnvironmentValues *values) -> EnvironmentLookupDependencies {
		return {
		    .context             = values,
		    .pam_environment     = LookupPam,
		    .process_environment = LookupProcess,
		};
	}

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
		ok &= expect(ExpectSsh(marker, true),
		             std::string(marker) + " is detected in PAM environment");
		ok &= expect(ExpectSsh(marker, false),
		             std::string(marker) + " is detected through process fallback");
	}

	EnvironmentValues values;
	values.pam["PAM_RHOST"]            = "remote-host";
	values.pam["SSH_CONNECTION_EXTRA"] = "prefixed";
	ok &= expect(!ProbeSessionState(nullptr, Dependencies(&values)).ssh_session,
	             "non-marker variables do not indicate SSH session");

	values.pam.clear();
	values.process.clear();
	values.pam_calls             = 0;
	values.process_calls         = 0;
	values.pam["SSH_CONNECTION"] = "first";
	values.pam["SSH_CLIENT"]     = "second";
	ok &= expect(ProbeSessionState(nullptr, Dependencies(&values)).ssh_session,
	             "multiple markers are detected");
	ok &= expect(values.pam_calls == 1 && values.process_calls == 0,
	             "first SSH marker short-circuits later lookups");

	values.pam.clear();
	values.pam["SSH_CLIENT"] = "";
	values.pam["SSH_TTY"]    = "tty";
	ok &= expect(ProbeSessionState(nullptr, Dependencies(&values)).ssh_session,
	             "empty and multiple marker values are detected");

	const EnvironmentLookupDependencies invalid_dependencies{};
	ok &= expect(!ProbeSessionState(nullptr, invalid_dependencies).ssh_session,
	             "invalid environment dependencies fail safely");

	return ok ? 0 : 1;
}
