
#include "runtime/runtime_session.hpp"
#include "storage/user_model_status.hpp"
#include "storage/user_models.hpp"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

#include <stdlib.h>

#include <sys/types.h>

namespace {
	auto WaitForRelease() -> bool {
		std::string command;
		return std::getline(std::cin, command) && command == "release";
	}
}  // namespace

auto main(int argc, char **argv) -> int {
	const auto arguments =
	    std::span(argv, argc > 0 ? static_cast<std::size_t>(argc) : std::size_t{0});
	const char *program = arguments.empty() ? "installed_runtime_e2e_driver" : arguments.front();
	if (arguments.size() != 5) {
		std::cerr << "Usage: " << program << " <--hold|--try> <user> <config> <models-directory>\n";
		return 2;
	}
	const std::string_view mode(arguments.subspan(1).front());
	if (mode != "--hold" && mode != "--try") {
		std::cerr << "Usage: " << program << " <--hold|--try> <user> <config> <models-directory>\n";
		return 2;
	}
	const char *user       = arguments.subspan(2).front();
	const char *config     = arguments.subspan(3).front();
	const char *models_dir = arguments.back();

	howdy::pam::RuntimeSession session(config, models_dir,
	                                   howdy::pam::ProductionRuntimeSessionOperations());
	const auto                 result = session.LoadForUser(user);
	if (!howdy::pam::RuntimeSessionLoadSucceeded(result) || !session.Staged()) {
		if (mode == "--try" &&
		    result.status == howdy::pam::RuntimeSessionLoadStatus::kPrepareFailed) {
			std::cerr << "Runtime staging failed\n";
		} else {
			std::cerr << "Runtime staging failed: session=" << static_cast<int>(result.status)
			          << ", config=" << static_cast<int>(result.config_result.status)
			          << ", errno=" << result.config_result.error_code
			          << ", message=" << result.config_result.error_message << '\n';
		}
		return 1;
	}
	if (setenv("HOWDY_USER_MODELS_DIR", session.UserModelsDir().c_str(), 1) != 0) {
		std::cerr << "Failed to select staged user-model directory: " << strerror(errno) << "\n";
		return 1;
	}
	const auto models =
	    howdy::native::LoadUserModels(user, "opencv_dnn_sface", static_cast<uid_t>(0));
	if (models.status != howdy::native::UserModelStatus::kOk || models.stored.encodings.empty()) {
		std::cerr << "Production user-model load failed: " << models.error_message << "\n";
		return 1;
	}

	std::cout << std::filesystem::path(session.ConfigPath()).parent_path().string() << '\n'
	          << std::flush;
	if (mode == "--hold" && !WaitForRelease()) {
		std::cerr << "Failed to read exact release command\n";
		return 1;
	}
	return 0;
}
