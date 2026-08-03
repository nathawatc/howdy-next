#include "runtime/environment.hpp"
#include "test_support.hpp"

#include <map>
#include <string>
#include <string_view>

namespace {

	using howdy::pam::runtime::EnvironmentLookupDependencies;
	using howdy::pam::runtime::EnvironmentSource;
	using howdy::pam::runtime::find_environment_variable;
	using howdy::test::expect;

	struct EnvironmentValues {
		std::map<std::string, std::string> pam;
		std::map<std::string, std::string> process;
		int                                pam_calls     = 0;
		int                                process_calls = 0;
	};

	auto lookup_pam(void *context, pam_handle_t *pamh, const char *name) -> const char * {
		(void)pamh;
		auto *values = static_cast<EnvironmentValues *>(context);
		++values->pam_calls;
		const auto found = values->pam.find(name == nullptr ? "" : name);
		return found == values->pam.end() ? nullptr : found->second.c_str();
	}

	auto lookup_process(void *context, const char *name) -> const char * {
		auto *values = static_cast<EnvironmentValues *>(context);
		++values->process_calls;
		const auto found = values->process.find(name == nullptr ? "" : name);
		return found == values->process.end() ? nullptr : found->second.c_str();
	}

	auto dependencies(EnvironmentValues *values) -> EnvironmentLookupDependencies {
		return {
		    .context             = values,
		    .pam_environment     = lookup_pam,
		    .process_environment = lookup_process,
		};
	}

}  // namespace

auto main() -> int {
	bool ok = true;

	EnvironmentValues values;
	const auto        deps = dependencies(&values);

	values.pam["SSH_CONNECTION"]     = "pam-value";
	values.process["SSH_CONNECTION"] = "process-value";
	ok &= expect(find_environment_variable(nullptr, "SSH_CONNECTION", deps) ==
	                 EnvironmentSource::kPam,
	             "PAM environment wins over process environment");
	ok &= expect(values.pam_calls == 1 && values.process_calls == 0,
	             "process environment is not consulted after PAM hit");

	values.pam.clear();
	values.process_calls = 0;
	ok &= expect(find_environment_variable(nullptr, "SSH_CONNECTION", deps) ==
	                 EnvironmentSource::kProcess,
	             "process environment is fallback after PAM miss");
	ok &= expect(values.process_calls == 1, "process fallback is consulted");

	values.process.clear();
	ok &= expect(find_environment_variable(nullptr, "SSH_CONNECTION", deps) ==
	                 EnvironmentSource::kMissing,
	             "missing variable returns missing source");

	values.pam["SSH_CONNECTION_EXTRA"] = "prefixed";
	ok &= expect(find_environment_variable(nullptr, "SSH_CONNECTION", deps) ==
	                 EnvironmentSource::kMissing,
	             "prefixed variable does not match exact name");

	values.pam["EMPTY"] = "";
	ok &= expect(find_environment_variable(nullptr, "EMPTY", deps) == EnvironmentSource::kPam,
	             "empty variable value still counts as present");

	const std::string embedded_null("EMPTY\0SUFFIX", 12);
	ok &= expect(find_environment_variable(nullptr, embedded_null, deps) ==
	                 EnvironmentSource::kMissing,
	             "embedded-null variable name is rejected");
	ok &= expect(find_environment_variable(nullptr, "", deps) == EnvironmentSource::kMissing,
	             "empty variable name is rejected");
	ok &= expect(find_environment_variable(nullptr, "INVALID=NAME", deps) ==
	                 EnvironmentSource::kMissing,
	             "variable name containing equals is rejected");
	const std::string oversized_name(64, 'A');
	ok &= expect(find_environment_variable(nullptr, oversized_name, deps) ==
	                 EnvironmentSource::kMissing,
	             "oversized variable name is rejected safely");

	const EnvironmentLookupDependencies no_process{
	    .context             = &values,
	    .pam_environment     = lookup_pam,
	    .process_environment = nullptr,
	};
	values.pam["PAM_ONLY"] = "yes";
	ok &= expect(find_environment_variable(nullptr, "PAM_ONLY", no_process) ==
	                 EnvironmentSource::kPam,
	             "null process callback safely permits PAM lookup");

	const EnvironmentLookupDependencies no_pam{
	    .context             = &values,
	    .pam_environment     = nullptr,
	    .process_environment = lookup_process,
	};
	values.process["FALLBACK"] = "yes";
	ok &= expect(find_environment_variable(nullptr, "FALLBACK", no_pam) ==
	                 EnvironmentSource::kProcess,
	             "null PAM callback safely permits process fallback");

	const EnvironmentLookupDependencies no_callbacks{
	    .context             = &values,
	    .pam_environment     = nullptr,
	    .process_environment = nullptr,
	};
	ok &= expect(find_environment_variable(nullptr, "FALLBACK", no_callbacks) ==
	                 EnvironmentSource::kMissing,
	             "null callbacks fail safely");

	const auto production = howdy::pam::runtime::production_environment_lookup_dependencies();
	ok &= expect(production.pam_environment != nullptr && production.process_environment != nullptr,
	             "production environment dependencies are complete");

	return ok ? 0 : 1;
}
