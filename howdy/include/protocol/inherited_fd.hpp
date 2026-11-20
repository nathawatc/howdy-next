#pragma once

#include <string_view>

namespace howdy::native {

	// PAM announces each descriptor it hands a child as NAME=N in the child's environment. The
	// protocol headers spell N out by hand; this keeps the name, value and entry in step with the
	// descriptor number, which must be a single digit.
	constexpr auto InheritedFdEntryMatches(int fd, std::string_view name, std::string_view value,
	                                       std::string_view entry) -> bool {
		return fd >= 0 && fd <= 9 && value.size() == 1 && value.front() == '0' + fd &&
		       !name.empty() && entry.size() == name.size() + 1 + value.size() &&
		       entry.starts_with(name) && entry.at(name.size()) == '=' && entry.ends_with(value);
	}

}  // namespace howdy::native
