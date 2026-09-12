#pragma once

#include "config/runtime_config.hpp"
#include "support/file_security/validation_root.hpp"

#include <filesystem>
#include <string>

namespace howdy::native::disable_internal {

	using ResolveConfigPathFn = std::filesystem::path (*)(void *context);

	using LoadRuntimeConfigFn = howdy::native::RuntimeConfigLoadResult (*)(
	    void *context, const std::filesystem::path &config_path);

	using UpdateConfigValueFn = bool (*)(void *context, const std::filesystem::path &config_path,
	                                     const std::string &key, const std::string &value,
	                                     std::string *error_message, bool lock,
	                                     bool validate_runtime);

	struct DisableDependencies {
		void               *context             = nullptr;
		ResolveConfigPathFn resolve_config_path = nullptr;
		LoadRuntimeConfigFn load_runtime_config = nullptr;
		UpdateConfigValueFn update_config_value = nullptr;
	};

	auto DisableMainWithDependencies(int argc, char **argv, const DisableDependencies &dependencies)
	    -> int;

	auto DisableMainWithValidationRoot(int argc, char **argv,
	                                   file_security_internal::ValidationRoot validation_root)
	    -> int;

}  // namespace howdy::native::disable_internal
