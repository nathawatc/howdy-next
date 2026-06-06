#include "cli/remove_cli.hpp"
#include "storage/user_models.hpp"

#include <charconv>
#include <iostream>
#include <string>
#include <string_view>

namespace {

    constexpr int kExitOk    = 0;
    constexpr int kExitAbort = 1;

    struct RemoveArgs {
        std::string user;
        std::string id;
        bool        yes = false;
    };

    auto parse_args(int argc, char **argv) -> RemoveArgs {
        RemoveArgs args;
        if (argc < 2) {
            std::exit(kExitAbort);
        }
        args.user = argv[1];
        for (int index = 2; index < argc; ++index) {
            const std::string_view arg(argv[index]);
            if (arg == "-y") {
                args.yes = true;
                continue;
            }
            if (args.id.empty()) {
                args.id = argv[index];
            }
        }
        return args;
    }

}  // namespace

int remove_main(int argc, char **argv) {
    const auto args = parse_args(argc, argv);
    if (args.id.empty()) {
        std::cout << "Please add the ID of the model you want to remove as an argument\n";
        std::cout << "For example:\n";
        std::cout << "\n\thowdy remove 0\n\n";
        std::cout << "You can find the IDs by running:\n";
        std::cout << "\n\thowdy list\n\n";
        return kExitAbort;
    }

    const auto models = howdy::native::list_user_model_entries(args.user, {});
    if (models.status == howdy::native::UserModelStatus::kNoModelDirectory) {
        std::cout << "Face models have not been initialized yet, please run:\n";
        std::cout << "\n\thowdy add\n\n";
        return kExitAbort;
    }
    if (models.status == howdy::native::UserModelStatus::kNoModel) {
        std::cout << "No face model known for the user " << args.user << ", please run:\n";
        std::cout << "\n\thowdy add\n\n";
        return kExitAbort;
    }
    if (models.status != howdy::native::UserModelStatus::kOk) {
        std::cout << models.error_message << "\n";
        return kExitAbort;
    }

    int id = -1;
    const auto [end, parse_error] =
        std::from_chars(args.id.data(), args.id.data() + args.id.size(), id);
    if (parse_error != std::errc() || end != args.id.data() + args.id.size()) {
        id = -1;
    }
    bool                                     found = false;
    std::string                              found_label;
    howdy::native::UserModelEntryExpectation expected;
    for (const auto &model : models.entries) {
        if (model.id == id && std::to_string(model.id) == args.id) {
            found       = true;
            found_label = model.label;
            expected    = howdy::native::UserModelEntryExpectation{
                .id      = model.id,
                .time    = model.time,
                .label   = model.label,
                .backend = model.backend,
                .metric  = model.metric,
                .model   = model.model,
            };
            break;
        }
    }

    if (!found) {
        std::cout << "No model with ID " << args.id << " exists for " << args.user << "\n";
        return kExitAbort;
    }

    if (!args.yes) {
        std::cout << "This will remove the model called \"" << found_label << "\" for " << args.user
                  << "\n";
        std::cout << "Do you want to continue [y/N]: ";
        std::string answer;
        std::getline(std::cin, answer);
        if (answer != "y" && answer != "Y") {
            std::cout << "\nInterpreting as a \"NO\", aborting\n";
            return kExitAbort;
        }
        std::cout << "\n";
    }

    const auto remove_result =
        howdy::native::remove_user_model_entry_if_matches(args.user, expected);
    if (remove_result.status != howdy::native::UserModelStatus::kOk) {
        std::cout << remove_result.error_message << "\n";
        return kExitAbort;
    }
    if (remove_result.removed_last) {
        std::cout << "Removed last model, howdy disabled for user\n";
        return kExitOk;
    }

    std::cout << "Removed model " << remove_result.entry.id << "\n";
    return kExitOk;
}
