#include "cli/disable.hpp"

#include "cli/disable/internal.hpp"
#include "config/config_schema.hpp"
#include "config/config_utils.hpp"
#include "config/runtime_config_loader.hpp"
#include "config/runtime_paths.hpp"

#include <iostream>
#include <string>

namespace {

	constexpr int kDisableExitOk    = 0;
	constexpr int kDisableExitAbort = 1;

	auto ResolveConfigPathDependency([[maybe_unused]] void *context) -> std::filesystem::path {
		return howdy::native::ResolveConfigPath();
	}

	auto LoadRuntimeConfigDependency(void *context, const std::filesystem::path &config_path)
	    -> howdy::native::RuntimeConfigLoadResult {
		return howdy::native::LoadRuntimeConfig(
		    config_path, howdy::native::DefaultSecureOwnerUid(),
		    *static_cast<howdy::native::file_security_internal::ValidationRoot *>(context));
	}

	auto UpdateConfigValueDependency(void *context, const std::filesystem::path &config_path,
	                                 const std::string &key, const std::string &value,
	                                 std::string *error_message, bool lock, bool validate_runtime)
	    -> bool {
		return howdy::native::UpdateConfigValue(
		    config_path, key, error_message, value, lock, validate_runtime,
		    *static_cast<howdy::native::file_security_internal::ValidationRoot *>(context));
	}

}  // namespace

auto howdy::native::disable_internal::DisableMainWithDependencies(
    const howdy::native::CommandInvocation &invocation, const DisableDependencies &dependencies)
    -> int {
	const std::string &argument_value = invocation.positionals.front();
	std::string        out_value;
	bool               disabled;
	if (argument_value == "1" || argument_value == "true") {
		out_value = "true";
		disabled  = true;
	} else if (argument_value == "0" || argument_value == "false") {
		out_value = "false";
		disabled  = false;
	} else {
		std::cout << "Invalid value; use 0 or false to enable, or 1 or true to disable Howdy\n";
		return kDisableExitAbort;
	}
	if (dependencies.resolve_config_path == nullptr ||
	    dependencies.load_runtime_config == nullptr ||
	    dependencies.update_config_value == nullptr) {
		return kDisableExitAbort;
	}

	const auto config_path   = dependencies.resolve_config_path(dependencies.context);
	auto       config_result = dependencies.load_runtime_config(dependencies.context, config_path);
	if (config_result.status != howdy::native::RuntimeConfigLoadStatus::kOk ||
	    !config_result.config.has_value()) {
		std::cerr << config_result.error_message << "\n";
		return kDisableExitAbort;
	}

	if (disabled == config_result.config->core.disabled) {
		std::cout << (disabled ? "Howdy is already disabled\n" : "Howdy is already enabled\n");
		return kDisableExitAbort;
	}

	const auto &disabled_option = howdy::native::config_schema::RuntimeConfigOption(
	    howdy::native::config_schema::OptionId::kCoreDisabled);
	std::string error_message;
	if (!dependencies.update_config_value(dependencies.context, config_path,
	                                      std::string(disabled_option.key), out_value,
	                                      &error_message, true, false)) {
		std::cout << (error_message.empty() ? "Failed to update \"disabled\" config option"
		                                    : error_message)
		          << "\n";
		return kDisableExitAbort;
	}

	std::cout << (disabled ? "Howdy is now disabled\n" : "Howdy is now enabled\n");
	return kDisableExitOk;
}

auto howdy::native::disable_internal::DisableMainWithValidationRoot(
    const howdy::native::CommandInvocation &invocation,
    file_security_internal::ValidationRoot  validation_root) -> int {
	return howdy::native::disable_internal::DisableMainWithDependencies(
	    invocation, {
	                    .context             = &validation_root,
	                    .resolve_config_path = ResolveConfigPathDependency,
	                    .load_runtime_config = LoadRuntimeConfigDependency,
	                    .update_config_value = UpdateConfigValueDependency,
	                });
}

auto DisableMain(const howdy::native::CommandInvocation &invocation) -> int {
	return howdy::native::disable_internal::DisableMainWithValidationRoot(invocation, {});
}
