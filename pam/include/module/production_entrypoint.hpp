#pragma once

#include "module/entrypoint.hpp"
#include "prompt/prompt_coordinator.hpp"
#include "runtime/compare_process.hpp"

namespace howdy::pam {

	__attribute__((visibility("hidden"))) auto
	ProductionPromptCoordinatorOperations(compare_process::Operations &compare_operations)
	    -> PromptCoordinatorOperations;

	__attribute__((visibility("hidden"))) auto
	ProductionAuthenticate(void *context, pam_handle_t *pamh, PamModuleArguments arguments,
	                       bool request_auth_token) -> int;

	__attribute__((visibility("hidden"))) auto ProductionEntrypointDependencies() noexcept
	    -> EntrypointDependencies;

}  // namespace howdy::pam
