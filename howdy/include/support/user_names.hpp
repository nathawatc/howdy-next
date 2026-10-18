#pragma once

#include <algorithm>
#include <cctype>
#include <string_view>

namespace howdy::native {

	inline constexpr auto kInvalidUserNameMessage =
	    "Invalid user name. Refusing to use an unsafe model file path.";

	inline auto IsValidModelUserName(const std::string_view user) -> bool {
		if (user.empty() || user == "." || user == ".." || user.size() > 255) {
			return false;
		}

		if (user.front() == '.' || user.front() == '-' || user.contains("..")) {
			return false;
		}

		return std::ranges::all_of(user, [](const char raw) -> bool {
			const auto ch = static_cast<unsigned char>(raw);
			return raw != '/' && raw != '\\' && std::isspace(ch) == 0 && std::iscntrl(ch) == 0;
		});
	}

}  // namespace howdy::native
