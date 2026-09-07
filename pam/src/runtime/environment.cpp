#include "runtime/environment.hpp"

#include <array>
#include <cstdlib>

namespace howdy::pam::runtime {

	namespace {

		auto ValidEnvironmentName(std::string_view name) -> bool {
			return !name.empty() && !name.contains('=') && !name.contains('\0');
		}

		auto ProductionPamEnvironment(void *context, pam_handle_t *pamh, const char *name) -> const
		    char * {
			(void)context;
			if (pamh == nullptr) {
				return nullptr;
			}
			return pam_getenv(pamh, name);
		}

		auto ProductionProcessEnvironment(void *context, const char *name) -> const char * {
			(void)context;
			return std::getenv(name);
		}

	}  // namespace

	auto ProductionEnvironmentLookupDependencies() -> EnvironmentLookupDependencies {
		return {
		    .context             = nullptr,
		    .pam_environment     = ProductionPamEnvironment,
		    .process_environment = ProductionProcessEnvironment,
		};
	}

	auto FindEnvironmentVariable(pam_handle_t *pamh, std::string_view name,
	                             const EnvironmentLookupDependencies &dependencies)
	    -> EnvironmentSource {
		constexpr std::size_t name_buffer_capacity = 64;
		if (!ValidEnvironmentName(name) || name.size() >= name_buffer_capacity ||
		    (dependencies.pam_environment == nullptr &&
		     dependencies.process_environment == nullptr)) {
			return EnvironmentSource::kMissing;
		}

		std::array<char, name_buffer_capacity> name_buffer{};
		for (std::size_t index = 0; index < name.size(); ++index) {
			name_buffer[index] = name[index];
		}

		if (dependencies.pam_environment != nullptr &&
		    dependencies.pam_environment(dependencies.context, pamh, name_buffer.data()) !=
		        nullptr) {
			return EnvironmentSource::kPam;
		}

		if (dependencies.process_environment != nullptr &&
		    dependencies.process_environment(dependencies.context, name_buffer.data()) != nullptr) {
			return EnvironmentSource::kProcess;
		}
		return EnvironmentSource::kMissing;
	}

}  // namespace howdy::pam::runtime
