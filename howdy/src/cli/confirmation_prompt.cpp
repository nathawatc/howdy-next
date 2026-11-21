#include "cli/confirmation_prompt.hpp"

#include <iostream>
#include <string>

namespace howdy::native::cli_internal {

	auto ConfirmInteractiveAction() -> bool {
		std::cout << "Continue? [y/N]: ";
		std::string answer;
		std::getline(std::cin, answer);
		if (answer != "y" && answer != "Y") {
			std::cout << "\nNo confirmation received; aborting.\n";
			return false;
		}
		return true;
	}

}  // namespace howdy::native::cli_internal
