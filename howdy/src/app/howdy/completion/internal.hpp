#pragma once

#include <string>
#include <vector>

namespace howdy::native::howdy_completion_internal {

	auto HandleCompletionQuery(const std::vector<std::string> &arguments, bool global_option_seen)
	    -> int;

}  // namespace howdy::native::howdy_completion_internal
