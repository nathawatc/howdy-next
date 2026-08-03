#include "runtime/environment.hpp"

#include <array>
#include <cstdlib>

namespace howdy::pam::runtime {

	namespace {

		auto valid_environment_name(std::string_view name) -> bool {
			return !name.empty() && !name.contains('=') && !name.contains('\0');
		}

		auto production_pam_environment(void *context, pam_handle_t *pamh, const char *name)
		    -> const char * {
			(void)context;
			if (pamh == nullptr) {
				return nullptr;
			}
			return pam_getenv(pamh, name);
		}

		auto production_process_environment(void *context, const char *name) -> const char * {
			(void)context;
			return std::getenv(name);
		}

	}  // namespace

	auto production_environment_lookup_dependencies() -> EnvironmentLookupDependencies {
		return {
		    .context             = nullptr,
		    .pam_environment     = production_pam_environment,
		    .process_environment = production_process_environment,
		};
	}

	auto find_environment_variable(pam_handle_t *pamh, std::string_view name,
	                               const EnvironmentLookupDependencies &dependencies)
	    -> EnvironmentSource {
		constexpr std::size_t kNameBufferCapacity = 64;
		if (!valid_environment_name(name) || name.size() >= kNameBufferCapacity ||
		    (dependencies.pam_environment == nullptr &&
		     dependencies.process_environment == nullptr)) {
			return EnvironmentSource::kMissing;
		}

		std::array<char, kNameBufferCapacity> name_buffer{};
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
