#include "cli/remove.hpp"

#include "cli/remove/internal.hpp"
#include "storage/user_model_status.hpp"
#include "storage/user_models.hpp"

#include <charconv>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace {

	constexpr int kRemoveExitOk    = 0;
	constexpr int kRemoveExitAbort = 1;

	struct RemoveArgs {
		std::string user;
		std::string id;
		bool        id_provided = false;
		bool        yes         = false;
	};

	auto ParseRemoveArgs(int argc, char **argv) -> std::optional<RemoveArgs> {
		RemoveArgs args;
		if (argc < 2) {
			return std::nullopt;
		}
		args.user          = argv[1];
		bool options_ended = false;
		for (int index = 2; index < argc; ++index) {
			const std::string_view arg(argv[index]);
			if (!options_ended && arg == "--") {
				options_ended = true;
				continue;
			}
			if (!options_ended && arg == "-y") {
				args.yes = true;
				continue;
			}
			if (!options_ended && !arg.empty() && arg.front() == '-') {
				return std::nullopt;
			}
			if (args.id_provided) {
				return std::nullopt;
			}
			args.id          = arg;
			args.id_provided = true;
		}
		return args;
	}

	auto RemoveCliUserModelEntriesDependency([[maybe_unused]] void *context,
	                                         const std::string     &user)
	    -> howdy::native::UserModelListResult {
		return howdy::native::ListUserModelEntries(user, {});
	}

	auto RemoveUserModelEntryIfMatchesDependency(
	    [[maybe_unused]] void *context, const std::string &user,
	    const howdy::native::UserModelEntryExpectation &expected)
	    -> howdy::native::UserModelMutationResult {
		return howdy::native::RemoveUserModelEntryIfMatches(user, expected);
	}

}  // namespace

auto howdy::native::remove_internal::RemoveMainWithDependencies(
    int argc, char **argv, const RemoveDependencies &dependencies) -> int {
	if (dependencies.list_user_model_entries == nullptr ||
	    dependencies.remove_user_model_entry_if_matches == nullptr) {
		return kRemoveExitAbort;
	}

	const auto args = ParseRemoveArgs(argc, argv);
	if (!args.has_value()) {
		return kRemoveExitAbort;
	}
	if (!args->id_provided) {
		std::cout << "Please specify the model ID to remove.\n";
		std::cout << "For example:\n";
		std::cout << "\n\thowdy remove 0\n\n";
		std::cout << "You can find the IDs by running:\n";
		std::cout << "\n\thowdy list\n\n";
		return kRemoveExitAbort;
	}

	const auto models = dependencies.list_user_model_entries(dependencies.context, args->user);
	if (models.status == howdy::native::UserModelStatus::kNoModelDirectory) {
		std::cout << "No face models found. Please run:\n";
		std::cout << "\n\thowdy add\n\n";
		return kRemoveExitAbort;
	}
	if (models.status == howdy::native::UserModelStatus::kNoModel) {
		std::cout << "No face models found. Please run:\n";
		std::cout << "\n\thowdy add\n\n";
		return kRemoveExitAbort;
	}
	if (models.status != howdy::native::UserModelStatus::kOk) {
		std::cout << models.error_message << "\n";
		return kRemoveExitAbort;
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
		return kRemoveExitAbort;
	}

	if (!args->yes) {
		std::cout << "Model \"" << found_label << "\" will be removed for " << args->user << ".\n";
		std::cout << "Continue? [y/N]: ";
		std::string answer;
		std::getline(std::cin, answer);
		if (answer != "y" && answer != "Y") {
			std::cout << "\nNo confirmation received; aborting.\n";
			return kRemoveExitAbort;
		}
		std::cout << "\n";
	}

	const auto remove_result =
	    dependencies.remove_user_model_entry_if_matches(dependencies.context, args->user, expected);
	if (remove_result.status != howdy::native::UserModelStatus::kOk) {
		std::cout << remove_result.error_message << "\n";
		return kRemoveExitAbort;
	}
	if (remove_result.removed_last) {
		std::cout << "Removed final face model; face verification disabled for this user\n";
		return kRemoveExitOk;
	}

	std::cout << "Removed model " << remove_result.entry.id << "\n";
	return kRemoveExitOk;
}

auto RemoveMain(int argc, char **argv) -> int {
	if (argc < 2) {
		return kRemoveExitAbort;
	}
	return howdy::native::remove_internal::RemoveMainWithDependencies(
	    argc, argv,
	    howdy::native::remove_internal::RemoveDependencies{
	        .list_user_model_entries            = RemoveCliUserModelEntriesDependency,
	        .remove_user_model_entry_if_matches = RemoveUserModelEntryIfMatchesDependency,
	    });
}
