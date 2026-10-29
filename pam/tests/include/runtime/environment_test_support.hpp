#pragma once

#include "runtime/environment.hpp"

#include <map>
#include <string>

#include <security/pam_appl.h>

namespace howdy::test::runtime_environment {

	struct EnvironmentValues {
		std::map<std::string, std::string> pam;
		std::map<std::string, std::string> process;
		int                                pam_calls     = 0;
		int                                process_calls = 0;
	};

	inline auto LookupPam(void *context, pam_handle_t *pamh, const char *name) -> const char * {
		(void)pamh;
		auto *values = static_cast<EnvironmentValues *>(context);
		++values->pam_calls;
		const auto found = values->pam.find(name == nullptr ? "" : name);
		return found == values->pam.end() ? nullptr : found->second.c_str();
	}

	inline auto LookupProcess(void *context, const char *name) -> const char * {
		auto *values = static_cast<EnvironmentValues *>(context);
		++values->process_calls;
		const auto found = values->process.find(name == nullptr ? "" : name);
		return found == values->process.end() ? nullptr : found->second.c_str();
	}

	inline auto Dependencies(EnvironmentValues *values)
	    -> howdy::pam::runtime::EnvironmentLookupDependencies {
		return {
		    .context             = values,
		    .pam_environment     = LookupPam,
		    .process_environment = LookupProcess,
		};
	}

}  // namespace howdy::test::runtime_environment
