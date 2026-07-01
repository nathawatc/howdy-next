#include "cli/clear_cli.hpp"
#include "cli/clear_internal.hpp"
#include "storage/user_models.hpp"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

	constexpr int kExitOk    = 0;
	constexpr int kExitAbort = 1;

	struct ClearArgs {
		std::string user;
		bool        yes = false;
	};

	auto parse_args(int argc, char **argv) -> std::optional<ClearArgs> {
		ClearArgs args;
		if (argc < 2) {
			return std::nullopt;
		}
		args.user = argv[1];
		for (int index = 2; index < argc; ++index) {
			if (std::string_view(argv[index]) == "-y") {
				args.yes = true;
			}
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
		std::cout << "No models created yet, can't clear them if they don't exist\n";
		return kExitAbort;
	}
	if (inspection.status == howdy::native::UserModelStatus::kNoModel) {
		std::cout << args->user << " has no models or they have been cleared already\n";
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

	if (!args->yes) {
		std::cout << "This will clear all models for " << args->user << "\n";
		std::cout << "Do you want to continue [y/N]: ";
		std::string answer;
		std::getline(std::cin, answer);
		if (answer != "y" && answer != "Y") {
			std::cout << "\nInterpreting as a \"NO\", aborting\n";
			return kExitAbort;
		}
	}

	const auto clear_result = dependencies.clear_user_model_entries_if_unchanged(
	    dependencies.context, args->user, *inspection.snapshot);
	if (clear_result.status == howdy::native::UserModelStatus::kNoModelDirectory) {
		std::cout << "No models created yet, can't clear them if they don't exist\n";
		return kExitAbort;
	}
	if (clear_result.status == howdy::native::UserModelStatus::kNoModel) {
		std::cout << args->user << " has no models or they have been cleared already\n";
		return kExitAbort;
	}
	if (clear_result.status != howdy::native::UserModelStatus::kOk) {
		std::cout << clear_result.error_message << "\n";
		return kExitAbort;
	}
	std::cout << "\nModels cleared\n";
	return kExitOk;
}

int clear_main(int argc, char **argv) {
	if (argc < 2) {
		std::exit(kExitAbort);
	}
	return howdy::native::clear_internal::clear_main_with_dependencies(
	    argc, argv,
	    {
	        .inspect_user_model_file = inspect_user_model_file_dependency,
	        .clear_user_model_entries_if_unchanged =
	            clear_user_model_entries_if_unchanged_dependency,
	    });
}
