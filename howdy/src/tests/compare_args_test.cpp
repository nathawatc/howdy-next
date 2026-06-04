#include "common/compare_args.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

    auto expect(bool condition, const std::string &message) -> bool {
        if (!condition) {
            std::cerr << "FAIL: " << message << "\n";
            return false;
        }
        return true;
    }

    auto argv_from(std::vector<std::string> &args) -> std::vector<char *> {
        std::vector<char *> result;
        result.reserve(args.size());
        for (auto &arg : args) {
            result.push_back(arg.data());
        }
        return result;
    }

}  // namespace

auto main() -> int {
    bool ok = true;

    {
        std::vector<std::string> args = {"howdy-compare", "alice"};
        auto                     argv = argv_from(args);
        const auto result = howdy::native::parse_compare_args(static_cast<int>(argv.size()),
                                                              argv.data(), "/tmp/config.ini");
        ok &= expect(result.status == howdy::native::CompareArgsStatus::kOk,
                     "simple user parse succeeds");
        ok &= expect(result.args.user == "alice", "user parsed");
        ok &= expect(result.args.config_path == "/tmp/config.ini", "default config path preserved");
    }

    {
        std::vector<std::string> args = {"howdy-compare", "--config", "/x.ini", "bob"};
        auto                     argv = argv_from(args);
        const auto result = howdy::native::parse_compare_args(static_cast<int>(argv.size()),
                                                              argv.data(), "/tmp/config.ini");
        ok &=
            expect(result.status == howdy::native::CompareArgsStatus::kOk, "config parse succeeds");
        ok &= expect(result.args.user == "bob", "user parsed after config");
        ok &= expect(result.args.config_path == "/x.ini", "custom config parsed");
    }

    {
        std::vector<std::string> args = {"howdy-compare", "--help"};
        auto                     argv = argv_from(args);
        const auto result = howdy::native::parse_compare_args(static_cast<int>(argv.size()),
                                                              argv.data(), "/tmp/config.ini");
        ok &= expect(result.status == howdy::native::CompareArgsStatus::kHelp,
                     "help produces help status");
        ok &= expect(result.exit_code == howdy::native::CompareExit::kSuccess,
                     "help returns success");
        ok &= expect(result.message.find("Usage: howdy-compare") != std::string::npos,
                     "help text populated");
    }

    {
        std::vector<std::string> args = {"howdy-compare", "--bad"};
        auto                     argv = argv_from(args);
        const auto result = howdy::native::parse_compare_args(static_cast<int>(argv.size()),
                                                              argv.data(), "/tmp/config.ini");
        ok &= expect(result.status == howdy::native::CompareArgsStatus::kError,
                     "unknown arg is error");
        ok &= expect(result.exit_code == howdy::native::CompareExit::kAbort, "unknown arg aborts");
        ok &= expect(result.message.find("Unknown argument: --bad") != std::string::npos,
                     "unknown arg message populated");
    }

    {
        std::vector<std::string> args = {"howdy-compare"};
        auto                     argv = argv_from(args);
        const auto result = howdy::native::parse_compare_args(static_cast<int>(argv.size()),
                                                              argv.data(), "/tmp/config.ini");
        ok &= expect(result.status == howdy::native::CompareArgsStatus::kError,
                     "missing user is error");
        ok &= expect(result.exit_code == howdy::native::CompareExit::kAbort, "missing user aborts");
    }

    if (!ok) {
        return 1;
    }
    return 0;
}
