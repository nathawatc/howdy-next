#pragma once

#include "app/command_invocation.hpp"
#include "cli/config/edit_session.hpp"

namespace howdy::native::config_internal {

	using ConfigDependencies = ConfigEditDependencies;

	auto ConfigMainWithDependencies(const CommandInvocation  &invocation,
	                                const ConfigDependencies &dependencies) -> int;

}  // namespace howdy::native::config_internal
