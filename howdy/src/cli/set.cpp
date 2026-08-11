#include "cli/set_cli.hpp"
#include "cli/set_internal.hpp"
#include "config/config_utils.hpp"
#include "config/runtime_paths.hpp"

#include <iostream>
#include <string>

namespace {

	constexpr int kExitOk    = 0;
	constexpr int kExitAbort = 1;

	auto resolve_config_path_dependency([[maybe_unused]] void *context) -> std::filesystem::path {
		return howdy::native::resolve_config_path();
	}

	auto update_config_value_dependency([[maybe_unused]] void       *context,
	                                    const std::filesystem::path &config_path,
	                                    const std::string &key, const std::string &value,
	                                    std::string *error_message, bool lock) -> bool {
		return howdy::native::update_config_value(config_path, key, error_message, value, lock);
	}

}  // namespace

auto howdy::native::set_internal::set_main_with_dependencies(int argc, char **argv,
                                                             const SetDependencies &dependencies)
    -> int {
	if (argc < 3) {
		std::cout << "Please specify a setting and value.\n";
		std::cout << "For example:\n";
		std::cout << "\n\thowdy set sface_threshold 0.363\n\n";
		return kExitAbort;
	}
	if (dependencies.resolve_config_path == nullptr ||
	    dependencies.update_config_value == nullptr) {
		return kExitAbort;
	}

	const auto        config_path = dependencies.resolve_config_path(dependencies.context);
	const std::string key         = argv[1];
	const std::string value       = argv[2];
	if (!howdy::native::is_safe_ini_scalar_value(value)) {
		std::cout << "Config values must be single-line scalars and cannot start with [\n";
		return kExitAbort;
	}
	std::string error_message;
	if (!dependencies.update_config_value(dependencies.context, config_path, key, value,
	                                      &error_message, true)) {
		std::cout << (error_message.empty() ? "Failed to update config option" : error_message)
		          << "\n";
		return kExitAbort;
	}

	std::cout << "Config option updated\n";
	return kExitOk;
}

auto set_main(int argc, char **argv) -> int {
	return howdy::native::set_internal::set_main_with_dependencies(
	    argc, argv,
	    howdy::native::set_internal::SetDependencies{
	        .resolve_config_path = resolve_config_path_dependency,
	        .update_config_value = update_config_value_dependency,
	    });
}
