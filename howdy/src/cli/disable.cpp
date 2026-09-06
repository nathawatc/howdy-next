#include "cli/disable.hpp"

#include "cli/disable/internal.hpp"
#include "config/config_schema.hpp"
#include "config/config_utils.hpp"
#include "config/runtime_config.hpp"
#include "config/runtime_paths.hpp"

#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

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

	auto parse_argument(int argc, char **argv) -> std::optional<std::string> {
		if (argc < 2) {
			return std::nullopt;
		}
		std::string argument;
		bool        argument_provided = false;
		bool        options_ended     = false;
		for (int index = 1; index < argc; ++index) {
			const std::string_view value(argv[index]);
			if (!options_ended && value == "--") {
				options_ended = true;
				continue;
			}
			if (!options_ended && !value.empty() && value.front() == '-') {
				return std::nullopt;
			}
			if (argument_provided) {
				return std::nullopt;
			}
			argument          = value;
			argument_provided = true;
		}
		return argument_provided ? std::optional<std::string>{std::move(argument)} : std::nullopt;
	}

}  // namespace

auto howdy::native::disable_internal::disable_main_with_dependencies(
    int argc, char **argv, const DisableDependencies &dependencies) -> int {
	if (argc < 2) {
		std::cout << "Specify 0 or false to enable, or 1 or true to disable Howdy\n";
		return kExitAbort;
	}
	const auto argument = parse_argument(argc, argv);
	if (!argument.has_value()) {
		std::cout << "Invalid arguments for disable\n";
		return kExitAbort;
	}

	const std::string &argument_value = *argument;
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
		std::cout << (disabled ? "Howdy is already disabled\n" : "Howdy is already enabled\n");
		return kExitAbort;
	}

	const auto &disabled_option = howdy::native::config_schema::runtime_config_option(
	    howdy::native::config_schema::OptionId::core_disabled);
	std::string error_message;
	if (!dependencies.update_config_value(dependencies.context, config_path,
	                                      std::string(disabled_option.key), out_value,
	                                      &error_message, true, false)) {
		std::cout << (error_message.empty() ? "Failed to update \"disabled\" config option"
		                                    : error_message)
		          << "\n";
		return kExitAbort;
	}

	std::cout << (disabled ? "Howdy is now disabled\n" : "Howdy is now enabled\n");
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
