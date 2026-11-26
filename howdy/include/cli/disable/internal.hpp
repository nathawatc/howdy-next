#pragma once

#include "app/command_invocation.hpp"
#include "config/config_utils.hpp"
#include "config/runtime_config_loader.hpp"
#include "support/file_security/validation_root.hpp"

#include <filesystem>

namespace howdy::native::disable_internal {

	using ResolveConfigPathFn = std::filesystem::path (*)(void *context);

	using LoadRuntimeConfigFn = howdy::native::RuntimeConfigLoadResult (*)(
	    void *context, const std::filesystem::path &config_path);

	using UpdateConfigValueFn = ConfigWriteResult (*)(void                        *context,
	                                                  const std::filesystem::path &config_path,
	                                                  const ConfigValueUpdate     &update);

	struct DisableDependencies {
		void               *context             = nullptr;
		ResolveConfigPathFn resolve_config_path = nullptr;
		LoadRuntimeConfigFn load_runtime_config = nullptr;
		UpdateConfigValueFn update_config_value = nullptr;
	};

	[[nodiscard]] auto DisableMainWithDependencies(const CommandInvocation   &invocation,
	                                               const DisableDependencies &dependencies) -> int;

	auto DisableMainWithValidationRoot(const CommandInvocation               &invocation,
	                                   file_security_internal::ValidationRoot validation_root)
	    -> int;

}  // namespace howdy::native::disable_internal
