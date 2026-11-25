#pragma once

#include "app/command_invocation.hpp"
#include "config/config_utils.hpp"

#include <filesystem>

namespace howdy::native::set_internal {

	using ResolveConfigPathFn = std::filesystem::path (*)(void *context);

	using UpdateConfigValueFn = ConfigWriteResult (*)(void                        *context,
	                                                  const std::filesystem::path &config_path,
	                                                  const ConfigValueUpdate     &update);

	struct SetDependencies {
		void               *context             = nullptr;
		ResolveConfigPathFn resolve_config_path = nullptr;
		UpdateConfigValueFn update_config_value = nullptr;
	};

	[[nodiscard]] auto SetMainWithDependencies(const CommandInvocation &invocation,
	                                           const SetDependencies   &dependencies) -> int;

}  // namespace howdy::native::set_internal
