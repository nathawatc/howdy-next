#include "cli/clear_cli.hpp"
#include "cli/clear_internal.hpp"
#include "storage/user_models.hpp"

#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

	constexpr int  kExitOk                   = 0;
	constexpr int  kExitAbort                = 1;
	constexpr auto kNoFaceModelsFoundMessage = "No face models found.";

	struct ClearArgs {
		std::string user;
		bool        yes = false;
	};

	auto parse_args(int argc, char **argv) -> std::optional<ClearArgs> {
		ClearArgs args;
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
			return std::nullopt;
		}
		return args;
	}

	auto inspect_user_model_file_dependency([[maybe_unused]] void *context, const std::string &user)
	    -> howdy::native::UserModelInspectResult {
		return howdy::native::inspect_user_model_file(user);
	}

	auto clear_user_model_entries_if_unchanged_dependency(
	    [[maybe_unused]] void *context, const std::string &user,
	    const howdy::native::UserModelFileSnapshot &expected_snapshot)
	    -> howdy::native::UserModelMutationResult {
		return howdy::native::clear_user_model_entries_if_unchanged(user, expected_snapshot);
	}

}  // namespace

auto howdy::native::clear_internal::clear_main_with_dependencies(
    int argc, char **argv, const ClearDependencies &dependencies) -> int {
	if (dependencies.inspect_user_model_file == nullptr ||
	    dependencies.clear_user_model_entries_if_unchanged == nullptr) {
		return kExitAbort;
	}

	const auto args = parse_args(argc, argv);
	if (!args.has_value()) {
		return kExitAbort;
	}

	const auto inspection = dependencies.inspect_user_model_file(dependencies.context, args->user);
	if (inspection.status == howdy::native::UserModelStatus::kNoModelDirectory) {
		std::cout << kNoFaceModelsFoundMessage << '\n';
		return kExitAbort;
	}
	if (inspection.status == howdy::native::UserModelStatus::kNoModel) {
		std::cout << kNoFaceModelsFoundMessage << '\n';
		return kExitAbort;
	}
	if (inspection.status != howdy::native::UserModelStatus::kOk) {
		std::cout << inspection.error_message << "\n";
		return kExitAbort;
	}
	if (!inspection.snapshot.has_value()) {
		std::cout << howdy::native::kUserModelFileInspectionFailedMessage << '\n';
		return kExitAbort;
	}

	if (!args->yes) {
		std::cout << "This will remove all face models for " << args->user << "\n";
		std::cout << "Continue? [y/N]: ";
		std::string answer;
		std::getline(std::cin, answer);
		if (answer != "y" && answer != "Y") {
			std::cout << "\nNo confirmation received; aborting.\n";
			return kExitAbort;
		}
	}

	const auto clear_result = dependencies.clear_user_model_entries_if_unchanged(
	    dependencies.context, args->user, *inspection.snapshot);
	if (clear_result.status == howdy::native::UserModelStatus::kNoModelDirectory) {
		std::cout << kNoFaceModelsFoundMessage << '\n';
		return kExitAbort;
	}
	if (clear_result.status == howdy::native::UserModelStatus::kNoModel) {
		std::cout << kNoFaceModelsFoundMessage << '\n';
		return kExitAbort;
	}
	if (clear_result.status != howdy::native::UserModelStatus::kOk) {
		std::cout << clear_result.error_message << "\n";
		return kExitAbort;
	}
	std::cout << "\nModels cleared\n";
	return kExitOk;
}

auto clear_main(int argc, char **argv) -> int {
	if (argc < 2) {
		return kExitAbort;
	}
	return howdy::native::clear_internal::clear_main_with_dependencies(
	    argc, argv,
	    {
	        .inspect_user_model_file = inspect_user_model_file_dependency,
	        .clear_user_model_entries_if_unchanged =
	            clear_user_model_entries_if_unchanged_dependency,
	    });
}
