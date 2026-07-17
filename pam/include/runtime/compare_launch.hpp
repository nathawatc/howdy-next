#pragma once

#include <string>

namespace howdy::pam {

	struct CompareLaunchRequest {
		std::string config_path;
		std::string username;
		std::string user_models_dir;
		bool        staged_runtime = false;
	};

}  // namespace howdy::pam
