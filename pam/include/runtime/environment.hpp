#pragma once

#include <cstdint>
#include <string_view>

#include <security/pam_appl.h>

namespace howdy::pam::runtime {

	enum class EnvironmentSource : std::uint8_t {
		kMissing,
		kPam,
		kProcess,
	};

	struct EnvironmentLookupDependencies {
		void *context                                                       = nullptr;
		const char *(*pam_environment)(void *context, pam_handle_t *pamh,
		                               const char *name)                    = nullptr;
		const char *(*process_environment)(void *context, const char *name) = nullptr;
	};

	auto production_environment_lookup_dependencies() -> EnvironmentLookupDependencies;

	auto find_environment_variable(pam_handle_t *pamh, std::string_view name,
	                               const EnvironmentLookupDependencies &dependencies)
	    -> EnvironmentSource;

}  // namespace howdy::pam::runtime
