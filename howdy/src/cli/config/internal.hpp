#pragma once

#include "support/invoking_user.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace howdy::native::config_internal {

	[[nodiscard]] auto RunEditor(const std::string           &editor_preference,
	                             const std::filesystem::path &temp_path,
	                             const std::optional<howdy::native::InvokingUser> &invoking_user)
	    -> int;

}  // namespace howdy::native::config_internal
