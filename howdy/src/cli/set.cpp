#include "cli/set.hpp"

#include "app/command_invocation.hpp"
#include "cli/set/internal.hpp"
#include "config/config_utils.hpp"
#include "config/runtime_paths.hpp"
#include "support/file_security/validation_root.hpp"

#include <filesystem>
#include <iostream>

namespace {

	constexpr int kSetExitOk    = 0;
	constexpr int kSetExitAbort = 1;

	auto SetCliResolveConfigPathDependency([[maybe_unused]] void *context)
	    -> std::filesystem::path {
		return howdy::native::ResolveConfigPath();
	}

	auto SetCliUpdateConfigValueDependency(void *context, const std::filesystem::path &config_path,
	                                       const howdy::native::ConfigValueUpdate &update)
	    -> howdy::native::ConfigWriteResult {
		return howdy::native::UpdateConfigValue(
		    config_path, update,
		    {
		        .validation_root =
		            *static_cast<howdy::native::file_security_internal::ValidationRoot *>(context),
		    });
	}

}  // namespace

auto howdy::native::set_internal::SetMainWithDependencies(
    const howdy::native::CommandInvocation &invocation, const SetDependencies &dependencies)
    -> int {
	if (dependencies.resolve_config_path == nullptr ||
	    dependencies.update_config_value == nullptr || invocation.positionals.size() < 2) {
		return kSetExitAbort;
	}

	const auto &config_path = dependencies.resolve_config_path(dependencies.context);
	const auto  result      = dependencies.update_config_value(
	    dependencies.context, config_path,
	    {.key = invocation.positionals.at(0), .value = invocation.positionals.at(1)});
	if (result.status != howdy::native::ConfigWriteStatus::kOk) {
		std::cout << (result.message.empty() ? "Failed to update config option" : result.message)
		          << "\n";
		return kSetExitAbort;
	}

	std::cout << "Config option updated\n";
	return kSetExitOk;
}

auto howdy::native::set_internal::SetMainWithValidationRoot(
    const howdy::native::CommandInvocation &invocation,
    file_security_internal::ValidationRoot  validation_root) -> int {
	return howdy::native::set_internal::SetMainWithDependencies(
	    invocation, {
	                    .context             = &validation_root,
	                    .resolve_config_path = SetCliResolveConfigPathDependency,
	                    .update_config_value = SetCliUpdateConfigValueDependency,
	                });
}

auto SetMain(const howdy::native::CommandInvocation &invocation) -> int {
	return howdy::native::set_internal::SetMainWithValidationRoot(invocation, {});
}
