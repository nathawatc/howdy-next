#pragma once

#include <string>
#include <system_error>

namespace howdy::native {

	[[nodiscard]] inline auto SystemErrorMessage(int error_number) -> std::string {
		return std::system_category().message(error_number);
	}

	[[nodiscard]] inline auto SystemErrorMessageWithNumber(int error_number) -> std::string {
		return SystemErrorMessage(error_number) + " (errno " + std::to_string(error_number) + ")";
	}

}  // namespace howdy::native
