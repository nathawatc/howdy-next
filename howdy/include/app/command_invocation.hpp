#pragma once

#include <optional>
#include <string>
#include <vector>

namespace howdy::native {

	struct CommandInvocation {
		std::optional<std::string> resolved_user;
		std::vector<std::string>   positionals;
		bool                       plain      = false;
		bool                       assume_yes = false;
		std::optional<std::string> device;
	};

}  // namespace howdy::native
