#pragma once

#include "support/user_names.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace howdy::native {

	inline auto ResolveUserModelPath(const std::filesystem::path &base_dir,
	                                 const std::string_view       user)
	    -> std::optional<std::filesystem::path> {
		if (!IsValidModelUserName(user)) {
			return std::nullopt;
		}

		return base_dir / (std::string(user) + ".dat");
	}

}  // namespace howdy::native
