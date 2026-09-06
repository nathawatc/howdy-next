
#include "runtime/runtime_session.hpp"
#include "storage/user_model_status.hpp"
#include "storage/user_models.hpp"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {
	auto wait_for_release() -> bool {
		std::string command;
		return std::getline(std::cin, command) && command == "release";
	}
}  // namespace

auto main(int argc, char **argv) -> int {
	if (argc != 5 ||
	    (std::string_view(argv[1]) != "--hold" && std::string_view(argv[1]) != "--try")) {
		std::cerr << "Usage: " << argv[0] << " <--hold|--try> <user> <config> <models-directory>\n";
		return 2;
	}

	howdy::pam::RuntimeSession session(argv[3], argv[4],
	                                   howdy::pam::production_runtime_session_dependencies());
	const auto                 result = session.load_for_user(argv[2]);
	if (!result.ok() || !session.staged()) {
		if (std::string_view(argv[1]) == "--try" &&
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
	if (setenv("HOWDY_USER_MODELS_DIR", session.user_models_dir().c_str(), 1) != 0) {
		std::cerr << "Failed to select staged user-model directory: " << strerror(errno) << "\n";
		return 1;
	}
	const auto models =
	    howdy::native::load_user_models(argv[2], "opencv_dnn_sface", static_cast<uid_t>(0));
	if (models.status != howdy::native::UserModelStatus::kOk || models.stored.encodings.empty()) {
		std::cerr << "Production user-model load failed: " << models.error_message << "\n";
		return 1;
	}

	std::cout << std::filesystem::path(session.config_path()).parent_path().string() << '\n'
	          << std::flush;
	if (std::string_view(argv[1]) == "--hold" && !wait_for_release()) {
		std::cerr << "Failed to read exact release command\n";
		return 1;
	}
	return 0;
}
