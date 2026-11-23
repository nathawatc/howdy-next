#include "compare/args.hpp"

#include "protocol/compare_exit.hpp"
#include "support/user_names.hpp"

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace howdy::native {

	auto ParseCompareArgs(int argc, char **argv) -> CompareArgsParseResult {
		CompareArgsParseResult result;
		if (argc <= 0 || argv == nullptr) {
			return result;
		}
		const auto arguments = std::span(argv, static_cast<std::size_t>(argc));
		if (std::ranges::any_of(arguments, [](const char *argument) -> bool {
			    return argument == nullptr;
		    })) {
			return result;
		}
		bool user_seen = false;

		if (arguments.size() == 2) {
			const std::string_view option(arguments.subspan(1, 1).front());
			if (option == "--help" || option == "-h") {
				result.status    = CompareArgsStatus::kHelp;
				result.exit_code = CompareExit::kSuccess;
				result.message   = "Usage: " + std::string(arguments.front()) + " <user>\n";
				return result;
			}
		}

		for (std::size_t index = 1; index < arguments.size(); ++index) {
			const std::string_view arg(arguments.subspan(index, 1).front());
			if (!arg.empty() && arg.front() == '-') {
				result.status    = CompareArgsStatus::kError;
				result.exit_code = CompareExit::kAbort;
				result.message   = "Unknown argument: " + std::string(arg) + "\n";
				return result;
			}
			if (user_seen) {
				result.status    = CompareArgsStatus::kError;
				result.exit_code = CompareExit::kAbort;
				result.message   = "Unexpected argument: " + std::string(arg) + "\n";
				return result;
			}
			result.args.user = arguments.subspan(index, 1).front();
			user_seen        = true;
		}

		if (result.args.user.empty()) {
			result.status    = CompareArgsStatus::kError;
			result.exit_code = CompareExit::kAbort;
			return result;
		}
		if (!IsValidModelUserName(result.args.user)) {
			result.status    = CompareArgsStatus::kError;
			result.exit_code = CompareExit::kAbort;
			result.message   = kInvalidUserNameMessage;
			return result;
		}

		result.status    = CompareArgsStatus::kOk;
		result.exit_code = CompareExit::kSuccess;
		return result;
	}

}  // namespace howdy::native
