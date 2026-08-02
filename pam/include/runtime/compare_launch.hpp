#pragma once

#include <string>

namespace howdy::pam {
	using CompareCancellationRequestedFn = bool (*)(void *context);

	struct CompareLaunchRequest {
		std::string config_path;
		std::string username;
		std::string user_models_dir;
		bool        staged_runtime = false;
	};

}  // namespace howdy::pam
