#pragma once

#include "cli/config/edit_session.hpp"
#include "support/invoking_user.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace howdy::native::config_internal {

	// Allow only a single bare command name or an absolute executable path (never whitespace,
	// embedded NUL, relative paths, trailing slashes, or '.'/'..' directory leaves).
	[[nodiscard]] inline auto IsSupportedEditorPreference(std::string_view editor) noexcept
	    -> bool {
		if (editor.empty() || editor.contains('\0') ||
		    editor.find_first_of(" \t\n\r\v\f") != std::string_view::npos) {
			return false;
		}
		const auto last_slash = editor.rfind('/');
		if (last_slash != std::string_view::npos && !editor.starts_with('/')) {
			return false;
		}
		const std::string_view leaf =
		    last_slash == std::string_view::npos ? editor : editor.substr(last_slash + 1);
		return !leaf.empty() && leaf != "." && leaf != "..";
	}

	[[nodiscard]] auto RunEditor(const std::string           &editor_preference,
	                             const std::filesystem::path &temp_path,
	                             const std::optional<howdy::native::InvokingUser> &invoking_user)
	    -> EditorRunResult;

}  // namespace howdy::native::config_internal
