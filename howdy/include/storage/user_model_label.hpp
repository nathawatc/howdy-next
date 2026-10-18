#pragma once

#include <algorithm>
#include <cctype>
#include <string_view>

namespace howdy::native {

	inline auto IsValidModelLabel(const std::string_view label) -> bool {
		return std::ranges::all_of(label, [](const char raw) -> bool {
			const auto ch = static_cast<unsigned char>(raw);
			return raw != '/' && raw != '\\' && std::iscntrl(ch) == 0;
		});
	}

}  // namespace howdy::native
