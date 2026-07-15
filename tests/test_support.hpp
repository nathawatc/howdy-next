#pragma once

#include <iostream>
#include <string_view>

namespace howdy::test {

	inline auto expect(bool condition, std::string_view message) -> bool {
		if (!condition) {
			std::cerr << "FAIL: " << message << '\n';
		}
		return condition;
	}

}  // namespace howdy::test
