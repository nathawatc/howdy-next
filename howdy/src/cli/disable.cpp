#include "cli/disable_cli.hpp"
#include "config/config_utils.hpp"
#include "config/runtime_config.hpp"
#include "config/runtime_paths.hpp"

#include <iostream>
#include <string>

namespace {

	constexpr int kExitOk    = 0;
	constexpr int kExitAbort = 1;

}  // namespace

int disable_main(int argc, char **argv) {
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

	const auto config_path   = howdy::native::resolve_config_path();
	auto       config_result = howdy::native::load_runtime_config(config_path);
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
	if (!howdy::native::update_config_value(config_path, "disabled", out_value, &error_message,
	                                        true, false)) {
		std::cout << (error_message.empty() ? "Failed to update \"disabled\" config option"
		                                    : error_message)
		          << "\n";
		return kExitAbort;
	}

	std::cout << (out_value == "true" ? "Howdy has been disabled\n" : "Howdy has been enabled\n");
	return kExitOk;
}
