#include "cli/set.hpp"

#include "cli/set/internal.hpp"
#include "config/config_utils.hpp"
#include "config/runtime_paths.hpp"

#include <iostream>
#include <string>

namespace {

	constexpr int kSetExitOk    = 0;
	constexpr int kSetExitAbort = 1;

	auto SetCliResolveConfigPathDependency([[maybe_unused]] void *context)
	    -> std::filesystem::path {
		return howdy::native::ResolveConfigPath();
	}

	auto UpdateConfigValueDependency([[maybe_unused]] void       *context,
	                                 const std::filesystem::path &config_path,
	                                 const std::string &key, const std::string &value,
	                                 std::string *error_message, bool lock) -> bool {
		return howdy::native::UpdateConfigValue(config_path, key, error_message, value, lock);
	}

}  // namespace

auto howdy::native::set_internal::SetMainWithDependencies(
    const howdy::native::CommandInvocation &invocation, const SetDependencies &dependencies)
    -> int {
	if (dependencies.resolve_config_path == nullptr ||
	    dependencies.update_config_value == nullptr) {
		return kSetExitAbort;
	}

	const auto &config_path = dependencies.resolve_config_path(dependencies.context);
	const auto &key         = invocation.positionals[0];
	const auto &value       = invocation.positionals[1];
	if (!howdy::native::IsSafeIniScalarValue(value)) {
		std::cout << "Config values must be single-line scalars and cannot start with [\n";
		return kSetExitAbort;
	}
	std::string error_message;
	if (!dependencies.update_config_value(dependencies.context, config_path, key, value,
	                                      &error_message, true)) {
		std::cout << (error_message.empty() ? "Failed to update config option" : error_message)
		          << "\n";
		return kSetExitAbort;
	}

	std::cout << "Config option updated\n";
	return kSetExitOk;
}

auto SetMain(const howdy::native::CommandInvocation &invocation) -> int {
	return howdy::native::set_internal::SetMainWithDependencies(
	    invocation, howdy::native::set_internal::SetDependencies{
	                    .resolve_config_path = SetCliResolveConfigPathDependency,
	                    .update_config_value = UpdateConfigValueDependency,
	                });
}
