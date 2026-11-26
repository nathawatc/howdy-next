#pragma once

#include "cli/config/edit_session.hpp"
#include "support/invoking_user.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace howdy::native::config_internal {

	[[nodiscard]] auto RunEditor(const std::string           &editor_preference,
	                             const std::filesystem::path &temp_path,
	                             const std::optional<howdy::native::InvokingUser> &invoking_user)
	    -> EditorRunResult;

}  // namespace howdy::native::config_internal
