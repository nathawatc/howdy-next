#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string_view>

namespace howdy::native {

	inline constexpr auto kInvalidUserNameMessage =
	    "Invalid user name. Refusing to use an unsafe model file path.";

	// A user's model file is named `<user><kUserModelFileSuffix>`.
	inline constexpr std::string_view kUserModelFileSuffix = ".dat";

	// The longest user name whose model file name still fits in NAME_MAX (255 bytes).
	inline constexpr std::size_t kMaxModelUserNameBytes = 255 - kUserModelFileSuffix.size();

	inline auto IsValidModelUserName(const std::string_view user) -> bool {
		if (user.empty() || user.size() > kMaxModelUserNameBytes) {
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
