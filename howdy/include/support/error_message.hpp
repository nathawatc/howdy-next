#pragma once

#include <string>
#include <system_error>

namespace howdy::native {

	[[nodiscard]] inline auto SystemErrorMessage(int error_number) -> std::string {
		return std::system_category().message(error_number);
	}

}  // namespace howdy::native
