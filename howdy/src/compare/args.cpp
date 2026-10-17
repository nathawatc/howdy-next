#include "compare/args.hpp"

#include "protocol/compare_exit.hpp"
#include "protocol/compare_invocation.hpp"
#include "support/user_names.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace howdy::native {

	auto ParseCompareArgs(int argc, char **argv, const std::string &default_config_path)
	    -> CompareArgsParseResult {
		CompareArgsParseResult result;
		result.args.config_path = default_config_path;
		bool       user_seen    = false;
		const auto arguments =
		    std::span(argv, argc > 0 ? static_cast<std::size_t>(argc) : std::size_t{0});

		if (arguments.size() == 2) {
			const std::string_view option(arguments.subspan(1, 1).front());
			if (option == "--help" || option == "-h") {
				result.status    = CompareArgsStatus::kHelp;
				result.exit_code = CompareExit::kSuccess;
				result.message =
				    "Usage: " + std::string(arguments.front()) + " [--config PATH] <user>\n";
				return result;
			}
		}

		for (std::size_t index = 1; index < arguments.size(); ++index) {
			const std::string_view arg(arguments.subspan(index, 1).front());
			if (arg == kCompareConfigOption && index + 1 < arguments.size()) {
				result.args.config_path = arguments.subspan(++index, 1).front();
				continue;
			}
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
