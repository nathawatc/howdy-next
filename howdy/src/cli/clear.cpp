#include "cli/clear.hpp"

#include "cli/clear/internal.hpp"
#include "storage/user_model_status.hpp"
#include "storage/user_models.hpp"

#include <iostream>
#include <string>

namespace {

	constexpr int  kClearExitOk              = 0;
	constexpr int  kClearExitAbort           = 1;
	constexpr auto kNoFaceModelsFoundMessage = "No face models found.";

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

auto howdy::native::clear_internal::ClearMainWithDependencies(
    const howdy::native::CommandInvocation &invocation, const ClearDependencies &dependencies)
    -> int {
	if (dependencies.inspect_user_model_file == nullptr ||
	    dependencies.clear_user_model_entries_if_unchanged == nullptr) {
		return kClearExitAbort;
	}

	if (!invocation.resolved_user.has_value()) {
		return kClearExitAbort;
	}
	const auto &user = *invocation.resolved_user;

	const auto inspection = dependencies.inspect_user_model_file(dependencies.context, user);
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

	if (!invocation.assume_yes) {
		std::cout << "This will remove all face models for " << user << "\n";
		std::cout << "Continue? [y/N]: ";
		std::string answer;
		std::getline(std::cin, answer);
		if (answer != "y" && answer != "Y") {
			std::cout << "\nNo confirmation received; aborting.\n";
			return kClearExitAbort;
		}
	}

	const auto clear_result = dependencies.clear_user_model_entries_if_unchanged(
	    dependencies.context, user, *inspection.snapshot);
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
    const howdy::native::CommandInvocation &invocation,
    file_security_internal::ValidationRoot  validation_root) -> int {
	return howdy::native::clear_internal::ClearMainWithDependencies(
	    invocation,
	    {
	        .context                               = &validation_root,
	        .inspect_user_model_file               = InspectUserModelFileDependency,
	        .clear_user_model_entries_if_unchanged = ClearUserModelEntriesIfUnchangedDependency,
	    });
}

auto ClearMain(const howdy::native::CommandInvocation &invocation) -> int {
	return howdy::native::clear_internal::ClearMainWithValidationRoot(invocation, {});
}
