#pragma once

#include "app/command_invocation.hpp"

#include <filesystem>
#include <string>

namespace howdy::native::set_internal {

	using ResolveConfigPathFn = std::filesystem::path (*)(void *context);

	using UpdateConfigValueFn = bool (*)(void *context, const std::filesystem::path &config_path,
	                                     const std::string &key, const std::string &value,
	                                     std::string *error_message, bool lock);

	struct SetDependencies {
		void               *context             = nullptr;
		ResolveConfigPathFn resolve_config_path = nullptr;
		UpdateConfigValueFn update_config_value = nullptr;
	};

	auto SetMainWithDependencies(const CommandInvocation &invocation,
	                             const SetDependencies   &dependencies) -> int;

}  // namespace howdy::native::set_internal
