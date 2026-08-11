#include "cli/remove_cli.hpp"
#include "cli/remove_internal.hpp"
#include "storage/user_models.hpp"

#include <charconv>
#include <iostream>
#include <optional>
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

	auto parse_args(int argc, char **argv) -> std::optional<RemoveArgs> {
		RemoveArgs args;
		if (argc < 2) {
			return std::nullopt;
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

	auto list_user_model_entries_dependency([[maybe_unused]] void *context, const std::string &user)
	    -> howdy::native::UserModelListResult {
		return howdy::native::list_user_model_entries(user, {});
	}

	auto remove_user_model_entry_if_matches_dependency(
	    [[maybe_unused]] void *context, const std::string &user,
	    const howdy::native::UserModelEntryExpectation &expected)
	    -> howdy::native::UserModelMutationResult {
		return howdy::native::remove_user_model_entry_if_matches(user, expected);
	}

}  // namespace

auto howdy::native::remove_internal::remove_main_with_dependencies(
    int argc, char **argv, const RemoveDependencies &dependencies) -> int {
	if (dependencies.list_user_model_entries == nullptr ||
	    dependencies.remove_user_model_entry_if_matches == nullptr) {
		return kExitAbort;
	}

	const auto args = parse_args(argc, argv);
	if (!args.has_value()) {
		return kExitAbort;
	}
	if (args->id.empty()) {
		std::cout << "Please specify the model ID to remove.\n";
		std::cout << "For example:\n";
		std::cout << "\n\thowdy remove 0\n\n";
		std::cout << "You can find the IDs by running:\n";
		std::cout << "\n\thowdy list\n\n";
		return kExitAbort;
	}

	const auto models = dependencies.list_user_model_entries(dependencies.context, args->user);
	if (models.status == howdy::native::UserModelStatus::kNoModelDirectory) {
		std::cout << "No face models found. Please run:\n";
		std::cout << "\n\thowdy add\n\n";
		return kExitAbort;
	}
	if (models.status == howdy::native::UserModelStatus::kNoModel) {
		std::cout << "No face models found. Please run:\n";
		std::cout << "\n\thowdy add\n\n";
		return kExitAbort;
	}
	if (models.status != howdy::native::UserModelStatus::kOk) {
		std::cout << models.error_message << "\n";
		return kExitAbort;
	}

	int id = -1;
	const auto [end, parse_error] =
	    std::from_chars(args->id.data(), args->id.data() + args->id.size(), id);
	if (parse_error != std::errc() || end != args->id.data() + args->id.size()) {
		id = -1;
	}
	bool                                     found = false;
	std::string                              found_label;
	howdy::native::UserModelEntryExpectation expected;
	for (const auto &model : models.entries) {
		if (model.id == id && std::to_string(model.id) == args->id) {
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
		std::cout << "No model with ID " << args->id << " exists for " << args->user << "\n";
		return kExitAbort;
	}

	if (!args->yes) {
		std::cout << "Model \"" << found_label << "\" will be removed for " << args->user << ".\n";
		std::cout << "Continue? [y/N]: ";
		std::string answer;
		std::getline(std::cin, answer);
		if (answer != "y" && answer != "Y") {
			std::cout << "\nNo confirmation received; aborting.\n";
			return kExitAbort;
		}
		std::cout << "\n";
	}

	const auto remove_result =
	    dependencies.remove_user_model_entry_if_matches(dependencies.context, args->user, expected);
	if (remove_result.status != howdy::native::UserModelStatus::kOk) {
		std::cout << remove_result.error_message << "\n";
		return kExitAbort;
	}
	if (remove_result.removed_last) {
		std::cout << "Removed final face model; face verification disabled for this user\n";
		return kExitOk;
	}

	std::cout << "Removed model " << remove_result.entry.id << "\n";
	return kExitOk;
}

auto remove_main(int argc, char **argv) -> int {
	if (argc < 2) {
		return kExitAbort;
	}
	return howdy::native::remove_internal::remove_main_with_dependencies(
	    argc, argv,
	    howdy::native::remove_internal::RemoveDependencies{
	        .list_user_model_entries            = list_user_model_entries_dependency,
	        .remove_user_model_entry_if_matches = remove_user_model_entry_if_matches_dependency,
	    });
}
