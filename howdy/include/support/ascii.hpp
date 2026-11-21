#pragma once

#include <algorithm>
#include <ranges>
#include <string_view>

namespace howdy::native {

	[[nodiscard]] constexpr auto ToLowerAscii(char character) noexcept -> char {
		if (character >= 'A' && character <= 'Z') {
			return static_cast<char>(character + ('a' - 'A'));
		}
		return character;
	}

	[[nodiscard]] constexpr auto AsciiEqualIgnoreCase(std::string_view left,
	                                                  std::string_view right) noexcept -> bool {
		return std::ranges::equal(left, right, [](char left_char, char right_char) -> bool {
			return ToLowerAscii(left_char) == ToLowerAscii(right_char);
		});
	}

	[[nodiscard]] constexpr auto AsciiStartsWithIgnoreCase(std::string_view value,
	                                                       std::string_view prefix) noexcept
	    -> bool {
		return value.size() >= prefix.size() &&
		       AsciiEqualIgnoreCase(value.substr(0, prefix.size()), prefix);
	}

}  // namespace howdy::native
