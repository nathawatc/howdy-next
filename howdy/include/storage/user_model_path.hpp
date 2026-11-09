#pragma once

#include "support/user_names.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace howdy::native {

	// Extension of a user's model file, `<user>.model`; the only definition of the name.
	inline constexpr std::string_view kUserModelFileExtension = ".model";

	inline auto UserModelFileName(const std::string_view user) -> std::optional<std::string> {
		if (!IsValidModelUserName(user)) {
			return std::nullopt;
		}
		return std::string(user) + std::string(kUserModelFileExtension);
	}

	inline auto ResolveUserModelPath(const std::filesystem::path &base_dir,
	                                 const std::string_view       user)
	    -> std::optional<std::filesystem::path> {
		auto file_name = UserModelFileName(user);
		if (!file_name.has_value()) {
			return std::nullopt;
		}
		return base_dir / *file_name;
	}

}  // namespace howdy::native
