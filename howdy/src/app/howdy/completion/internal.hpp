#pragma once

#include <string>
#include <vector>

namespace howdy::native::howdy_completion_internal {

	auto handle_completion_query(const std::vector<std::string> &arguments, bool global_option_seen)
	    -> int;

}  // namespace howdy::native::howdy_completion_internal
