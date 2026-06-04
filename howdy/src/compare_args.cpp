#include "common/compare_args.hpp"

#include <string_view>

namespace howdy::native {

    auto parse_compare_args(int argc, char **argv, const std::string &default_config_path)
        -> CompareArgsParseResult {
        CompareArgsParseResult result;
        result.args.config_path = default_config_path;

        for (int index = 1; index < argc; ++index) {
            const std::string_view arg(argv[index]);
            if (arg == "--help" || arg == "-h") {
                result.status    = CompareArgsStatus::kHelp;
                result.exit_code = CompareExit::kSuccess;
                result.message   = "Usage: " + std::string(argv[0]) + " [--config PATH] <user>\n";
                return result;
            }
            if (arg == "--config" && index + 1 < argc) {
                result.args.config_path = argv[++index];
                continue;
            }
            if (!arg.empty() && arg.front() == '-') {
                result.status    = CompareArgsStatus::kError;
                result.exit_code = CompareExit::kAbort;
                result.message   = "Unknown argument: " + std::string(arg) + "\n";
                return result;
            }
            result.args.user = argv[index];
        }

        if (result.args.user.empty()) {
            result.status    = CompareArgsStatus::kError;
            result.exit_code = CompareExit::kAbort;
            return result;
        }

        result.status    = CompareArgsStatus::kOk;
        result.exit_code = CompareExit::kSuccess;
        return result;
    }

}  // namespace howdy::native
