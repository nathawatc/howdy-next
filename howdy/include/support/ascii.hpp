#pragma once

#include <algorithm>
#include <string_view>

namespace howdy::native {

	[[nodiscard]] constexpr auto IsAsciiControl(char character) noexcept -> bool {
		const auto code = static_cast<unsigned char>(character);
		return code < 0x20U || code == 0x7FU;
	}

	[[nodiscard]] constexpr auto IsAsciiSpace(char character) noexcept -> bool {
		return character == ' ' || character == '\t' || character == '\n' || character == '\r' ||
		       character == '\v' || character == '\f';
	}

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
