#include "cli/clear.hpp"

#include "cli/clear/internal.hpp"
#include "storage/user_model_status.hpp"
#include "storage/user_models.hpp"

#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

	constexpr int  kClearExitOk              = 0;
	constexpr int  kClearExitAbort           = 1;
	constexpr auto kNoFaceModelsFoundMessage = "No face models found.";

	struct ClearArgs {
		std::string user;
		bool        yes = false;
	};

	auto ParseClearArgs(int argc, char **argv) -> std::optional<ClearArgs> {
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

	auto InspectUserModelFileDependency(void *context, const std::string &user)
	    -> howdy::native::UserModelInspectResult {
		return howdy::native::InspectUserModelFile(
		    user, *static_cast<howdy::native::file_security_internal::ValidationRoot *>(context));
	}

	auto ClearUserModelEntriesIfUnchangedDependency(
	    void *context, const std::string &user,
	    const howdy::native::UserModelFileSnapshot &expected_snapshot)
	    -> howdy::native::UserModelMutationResult {
		return howdy::native::ClearUserModelEntriesIfUnchanged(
		    user, expected_snapshot,
		    *static_cast<howdy::native::file_security_internal::ValidationRoot *>(context));
	}

}  // namespace

auto howdy::native::clear_internal::ClearMainWithDependencies(int argc, char **argv,
                                                              const ClearDependencies &dependencies)
    -> int {
	if (dependencies.inspect_user_model_file == nullptr ||
	    dependencies.clear_user_model_entries_if_unchanged == nullptr) {
		return kClearExitAbort;
	}

	const auto args = ParseClearArgs(argc, argv);
	if (!args.has_value()) {
		return kClearExitAbort;
	}

	const auto inspection = dependencies.inspect_user_model_file(dependencies.context, args->user);
	if (inspection.status == howdy::native::UserModelStatus::kNoModelDirectory) {
		std::cout << kNoFaceModelsFoundMessage << '\n';
		return kClearExitAbort;
	}
	if (inspection.status == howdy::native::UserModelStatus::kNoModel) {
		std::cout << kNoFaceModelsFoundMessage << '\n';
		return kClearExitAbort;
	}
	if (inspection.status != howdy::native::UserModelStatus::kOk) {
		std::cout << inspection.error_message << "\n";
		return kClearExitAbort;
	}
	if (!inspection.snapshot.has_value()) {
		std::cout << howdy::native::kUserModelFileInspectionFailedMessage << '\n';
		return kClearExitAbort;
	}

	if (!args->yes) {
		std::cout << "This will remove all face models for " << args->user << "\n";
		std::cout << "Continue? [y/N]: ";
		std::string answer;
		std::getline(std::cin, answer);
		if (answer != "y" && answer != "Y") {
			std::cout << "\nNo confirmation received; aborting.\n";
			return kClearExitAbort;
		}
	}

	const auto clear_result = dependencies.clear_user_model_entries_if_unchanged(
	    dependencies.context, args->user, *inspection.snapshot);
	if (clear_result.status == howdy::native::UserModelStatus::kNoModelDirectory) {
		std::cout << kNoFaceModelsFoundMessage << '\n';
		return kClearExitAbort;
	}
	if (clear_result.status == howdy::native::UserModelStatus::kNoModel) {
		std::cout << kNoFaceModelsFoundMessage << '\n';
		return kClearExitAbort;
	}
	if (clear_result.status != howdy::native::UserModelStatus::kOk) {
		std::cout << clear_result.error_message << "\n";
		return kClearExitAbort;
	}
	std::cout << "\nModels cleared\n";
	return kClearExitOk;
}

auto howdy::native::clear_internal::ClearMainWithValidationRoot(
    int argc, char **argv, file_security_internal::ValidationRoot validation_root) -> int {
	if (argc < 2) {
		return kClearExitAbort;
	}
	return howdy::native::clear_internal::ClearMainWithDependencies(
	    argc, argv,
	    {
	        .context                               = &validation_root,
	        .inspect_user_model_file               = InspectUserModelFileDependency,
	        .clear_user_model_entries_if_unchanged = ClearUserModelEntriesIfUnchangedDependency,
	    });
}

auto ClearMain(int argc, char **argv) -> int {
	return howdy::native::clear_internal::ClearMainWithValidationRoot(argc, argv, {});
}
