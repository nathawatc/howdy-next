#pragma once

#include "cli/config/edit_session.hpp"

namespace howdy::native::config_internal {

	using ConfigDependencies = ConfigEditDependencies;

	auto ConfigMainWithDependencies(int argc, char **argv, const ConfigDependencies &dependencies)
	    -> int;

}  // namespace howdy::native::config_internal
