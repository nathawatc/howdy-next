#pragma once

#include "app/command_invocation.hpp"
#include "cli/config/edit_session.hpp"

namespace howdy::native::config_internal {

	auto ConfigMainWithDependencies(const CommandInvocation      &invocation,
	                                const ConfigEditDependencies &dependencies) -> int;

}  // namespace howdy::native::config_internal
