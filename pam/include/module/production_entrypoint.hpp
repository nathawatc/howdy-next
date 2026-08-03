#pragma once

#include "module/entrypoint.hpp"

namespace howdy::pam {

	__attribute__((visibility("hidden"))) auto production_entrypoint_dependencies() noexcept
	    -> EntrypointDependencies;

}  // namespace howdy::pam
