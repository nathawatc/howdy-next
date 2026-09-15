#include "runtime/environment.hpp"
#include "test_support.hpp"

#include <map>
#include <string>
#include <string_view>

namespace {

	using howdy::pam::runtime::EnvironmentLookupDependencies;
	using howdy::pam::runtime::EnvironmentSource;
	using howdy::pam::runtime::FindEnvironmentVariable;
	using howdy::test::Expect;

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

}  // namespace

auto main() -> int {
	bool ok = true;

	EnvironmentValues values;
	const auto        deps = Dependencies(&values);

	values.pam["SSH_CONNECTION"]     = "pam-value";
	values.process["SSH_CONNECTION"] = "process-value";
	ok &=
	    Expect(FindEnvironmentVariable(nullptr, "SSH_CONNECTION", deps) == EnvironmentSource::kPam,
	           "PAM environment wins over process environment");
	ok &= Expect(values.pam_calls == 1 && values.process_calls == 0,
	             "process environment is not consulted after PAM hit");

	values.pam.clear();
	values.process_calls = 0;
	ok &= Expect(FindEnvironmentVariable(nullptr, "SSH_CONNECTION", deps) ==
	                 EnvironmentSource::kProcess,
	             "process environment is fallback after PAM miss");
	ok &= Expect(values.process_calls == 1, "process fallback is consulted");

	values.process.clear();
	ok &= Expect(FindEnvironmentVariable(nullptr, "SSH_CONNECTION", deps) ==
	                 EnvironmentSource::kMissing,
	             "missing variable returns missing source");

	values.pam["SSH_CONNECTION_EXTRA"] = "prefixed";
	ok &= Expect(FindEnvironmentVariable(nullptr, "SSH_CONNECTION", deps) ==
	                 EnvironmentSource::kMissing,
	             "prefixed variable does not match exact name");

	values.pam["EMPTY"] = "";
	ok &= Expect(FindEnvironmentVariable(nullptr, "EMPTY", deps) == EnvironmentSource::kPam,
	             "empty variable value still counts as present");

	const std::string embedded_null("EMPTY\0SUFFIX", 12);
	ok &=
	    Expect(FindEnvironmentVariable(nullptr, embedded_null, deps) == EnvironmentSource::kMissing,
	           "embedded-null variable name is rejected");
	ok &= Expect(FindEnvironmentVariable(nullptr, "", deps) == EnvironmentSource::kMissing,
	             "empty variable name is rejected");
	ok &= Expect(FindEnvironmentVariable(nullptr, "INVALID=NAME", deps) ==
	                 EnvironmentSource::kMissing,
	             "variable name containing equals is rejected");
	const std::string oversized_name(64, 'A');
	ok &= Expect(FindEnvironmentVariable(nullptr, oversized_name, deps) ==
	                 EnvironmentSource::kMissing,
	             "oversized variable name is rejected safely");

	const EnvironmentLookupDependencies no_process{
	    .context             = &values,
	    .pam_environment     = LookupPam,
	    .process_environment = nullptr,
	};
	values.pam["PAM_ONLY"] = "yes";
	ok &=
	    Expect(FindEnvironmentVariable(nullptr, "PAM_ONLY", no_process) == EnvironmentSource::kPam,
	           "null process callback safely permits PAM lookup");

	const EnvironmentLookupDependencies no_pam{
	    .context             = &values,
	    .pam_environment     = nullptr,
	    .process_environment = LookupProcess,
	};
	values.process["FALLBACK"] = "yes";
	ok &=
	    Expect(FindEnvironmentVariable(nullptr, "FALLBACK", no_pam) == EnvironmentSource::kProcess,
	           "null PAM callback safely permits process fallback");

	const EnvironmentLookupDependencies no_callbacks{
	    .context             = &values,
	    .pam_environment     = nullptr,
	    .process_environment = nullptr,
	};
	ok &= Expect(FindEnvironmentVariable(nullptr, "FALLBACK", no_callbacks) ==
	                 EnvironmentSource::kMissing,
	             "null callbacks fail safely");

	const auto production = howdy::pam::runtime::ProductionEnvironmentLookupDependencies();
	ok &= Expect(production.pam_environment != nullptr && production.process_environment != nullptr,
	             "production environment dependencies are complete");

	return ok ? 0 : 1;
}
