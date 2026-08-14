#pragma once

#include <string>
#include <string_view>

namespace howdy::native {

	inline auto format_version(std::string_view project_version, std::string_view build_commit)
	    -> std::string {
		std::string output = "Howdy Next ";
		output += project_version;
		if (!build_commit.empty()) {
			output += " (";
			output += build_commit;
			output += ')';
		}
		return output;
	}

}  // namespace howdy::native
