#pragma once

#include "module/entrypoint.hpp"

namespace howdy::pam {

	__attribute__((visibility("hidden"))) auto ProductionEntrypointDependencies() noexcept
	    -> EntrypointDependencies;

}  // namespace howdy::pam
