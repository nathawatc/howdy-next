#include "cli/clear_cli.hpp"
#include "storage/user_models.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

    constexpr int kExitOk    = 0;
    constexpr int kExitAbort = 1;

    struct ClearArgs {
        std::string user;
        bool        yes = false;
    };

    auto parse_args(int argc, char **argv) -> ClearArgs {
        ClearArgs args;
        if (argc < 2) {
            std::exit(kExitAbort);
        }
        args.user = argv[1];
        for (int index = 2; index < argc; ++index) {
            if (std::string_view(argv[index]) == "-y") {
                args.yes = true;
            }
        }
        return args;
    }

}  // namespace

int clear_main(int argc, char **argv) {
    const auto args       = parse_args(argc, argv);
    const auto inspection = howdy::native::inspect_user_model_file(args.user);
    if (inspection.status == howdy::native::UserModelStatus::kNoModelDirectory) {
        std::cout << "No models created yet, can't clear them if they don't exist\n";
        return kExitAbort;
    }
    if (inspection.status == howdy::native::UserModelStatus::kNoModel) {
        std::cout << args.user << " has no models or they have been cleared already\n";
        return kExitAbort;
    }
    if (inspection.status != howdy::native::UserModelStatus::kOk) {
        std::cout << inspection.error_message << "\n";
        return kExitAbort;
    }
    if (!inspection.snapshot.has_value()) {
        std::cout << "Failed to inspect user model file\n";
        return kExitAbort;
    }

    if (!args.yes) {
        std::cout << "This will clear all models for " << args.user << "\n";
        std::cout << "Do you want to continue [y/N]: ";
        std::string answer;
        std::getline(std::cin, answer);
        if (answer != "y" && answer != "Y") {
            std::cout << "\nInterpreting as a \"NO\", aborting\n";
            return kExitAbort;
        }
    }

    const auto clear_result =
        howdy::native::clear_user_model_entries_if_unchanged(args.user, *inspection.snapshot);
    if (clear_result.status == howdy::native::UserModelStatus::kNoModelDirectory) {
        std::cout << "No models created yet, can't clear them if they don't exist\n";
        return kExitAbort;
    }
    if (clear_result.status == howdy::native::UserModelStatus::kNoModel) {
        std::cout << args.user << " has no models or they have been cleared already\n";
        return kExitAbort;
    }
    if (clear_result.status != howdy::native::UserModelStatus::kOk) {
        std::cout << clear_result.error_message << "\n";
        return kExitAbort;
    }
    std::cout << "\nModels cleared\n";
    return kExitOk;
}
