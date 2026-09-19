#pragma once

#include <cerrno>
#include <concepts>
#include <utility>

namespace howdy::native {
	template <typename Operation>
	    requires std::signed_integral<decltype(std::declval<Operation &>()())>
	inline auto RetryOnEintr(Operation operation) -> decltype(operation()) {
		auto result = operation();
		while (result < 0 && errno == EINTR) {
			result = operation();
		}
		return result;
	}
}  // namespace howdy::native
