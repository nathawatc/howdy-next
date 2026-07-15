#include "cli/disable_cli.hpp"
#include "cli/disable_internal.hpp"
#include "config/config_utils.hpp"
#include "config/runtime_config.hpp"
#include "config/runtime_paths.hpp"

#include <iostream>
#include <string>

namespace {

	constexpr int kExitOk    = 0;
	constexpr int kExitAbort = 1;

	auto resolve_config_path_dependency([[maybe_unused]] void *context) -> std::filesystem::path {
		return howdy::native::resolve_config_path();
	}

	auto load_runtime_config_dependency([[maybe_unused]] void       *context,
	                                    const std::filesystem::path &config_path)
	    -> howdy::native::RuntimeConfigLoadResult {
		return howdy::native::load_runtime_config(config_path);
	}

	auto update_config_value_dependency([[maybe_unused]] void       *context,
	                                    const std::filesystem::path &config_path,
	                                    const std::string &key, const std::string &value,
	                                    std::string *error_message, bool lock,
	                                    bool validate_runtime) -> bool {
		return howdy::native::update_config_value(config_path, key, error_message, value, lock,
		                                          validate_runtime);
	}

}  // namespace

auto howdy::native::disable_internal::disable_main_with_dependencies(
    int argc, char **argv, const DisableDependencies &dependencies) -> int {
	if (argc < 2) {
		std::cout << "Please add a 0 (enable) or a 1 (disable) as an argument\n";
		return kExitAbort;
	}

	const std::string argument = argv[1];
	std::string       out_value;
	bool              disabled;
	if (argument == "1" || argument == "true") {
		out_value = "true";
		disabled  = true;
	} else if (argument == "0" || argument == "false") {
		out_value = "false";
		disabled  = false;
	} else {
		std::cout << "Please only use 0 (enable) or 1 (disable) as an argument\n";
		return kExitAbort;
	}
	if (dependencies.resolve_config_path == nullptr ||
	    dependencies.load_runtime_config == nullptr ||
	    dependencies.update_config_value == nullptr) {
		return kExitAbort;
	}

	const auto config_path   = dependencies.resolve_config_path(dependencies.context);
	auto       config_result = dependencies.load_runtime_config(dependencies.context, config_path);
	if (config_result.status != howdy::native::RuntimeConfigLoadStatus::kOk ||
	    !config_result.config.has_value()) {
		std::cerr << config_result.error_message << "\n";
		return kExitAbort;
	}

	if (disabled == config_result.config->core.disabled) {
		std::cout << "The disable option has already been set to " << out_value << "\n";
		return kExitAbort;
	}

	std::string error_message;
	if (!dependencies.update_config_value(dependencies.context, config_path, "disabled", out_value,
	                                      &error_message, true, false)) {
		std::cout << (error_message.empty() ? "Failed to update \"disabled\" config option"
		                                    : error_message)
		          << "\n";
		return kExitAbort;
	}

	std::cout << (out_value == "true" ? "Howdy has been disabled\n" : "Howdy has been enabled\n");
	return kExitOk;
}

auto disable_main(int argc, char **argv) -> int {
	return howdy::native::disable_internal::disable_main_with_dependencies(
	    argc, argv,
	    {
	        .resolve_config_path = resolve_config_path_dependency,
	        .load_runtime_config = load_runtime_config_dependency,
	        .update_config_value = update_config_value_dependency,
	    });
}
