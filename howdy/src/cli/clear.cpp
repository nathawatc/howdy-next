#include "cli/clear.hpp"

#include "app/command_invocation.hpp"
#include "cli/clear/internal.hpp"
#include "cli/confirmation_prompt.hpp"
#include "storage/user_model_status.hpp"
#include "storage/user_model_store.hpp"
#include "storage/user_model_types.hpp"

#include <iostream>
#include <string>

namespace {

	constexpr int  kClearExitOk              = 0;
	constexpr int  kClearExitAbort           = 1;
	constexpr auto kNoFaceModelsFoundMessage = "No face models found.";

	auto InspectUserModelFileFromStore(void *context, const std::string &user)
	    -> howdy::native::UserModelInspectResult {
		const auto *store = static_cast<const howdy::native::UserModelStore *>(context);
		if (store == nullptr) {
			return {
			    .status        = howdy::native::UserModelStatus::kParseError,
			    .error_message = "User model store is not initialized",
			};
		}
		return store->InspectFile(user);
	}

	auto ClearUserModelEntriesFromStore(void *context, const std::string &user,
	                                    const howdy::native::UserModelFileSnapshot &expected)
	    -> howdy::native::UserModelMutationResult {
		const auto *store = static_cast<const howdy::native::UserModelStore *>(context);
		if (store == nullptr) {
			return {
			    .status        = howdy::native::UserModelStatus::kWriteFailed,
			    .error_message = "User model store is not initialized",
			};
		}
		return store->ClearEntriesIfUnchanged(user, expected);
	}

}  // namespace

auto howdy::native::clear_internal::StoreDependencies(howdy::native::UserModelStore &store)
    -> ClearDependencies {
	return {
	    .context                               = &store,
	    .inspect_user_model_file               = InspectUserModelFileFromStore,
	    .clear_user_model_entries_if_unchanged = ClearUserModelEntriesFromStore,
	};
}

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
	const auto &user       = *invocation.resolved_user;
	const auto  inspection = dependencies.inspect_user_model_file(dependencies.context, user);
	if (howdy::native::Classify(inspection.status) == howdy::native::UserModelOutcome::kAbsent) {
		std::cout << kNoFaceModelsFoundMessage << '\n';
		return kClearExitOk;
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
		if (!howdy::native::cli_internal::ConfirmInteractiveAction()) {
			return kClearExitAbort;
		}
	}

	const auto clear_result = dependencies.clear_user_model_entries_if_unchanged(
	    dependencies.context, user, *inspection.snapshot);
	// The store reports a model gone since the inspection as changed; an absent result can
	// only come from another adapter at this seam, and still means nothing is left to clear.
	if (howdy::native::Classify(clear_result.status) == howdy::native::UserModelOutcome::kAbsent) {
		std::cout << kNoFaceModelsFoundMessage << '\n';
		return kClearExitOk;
	}
	if (clear_result.status != howdy::native::UserModelStatus::kOk) {
		std::cout << clear_result.error_message << "\n";
	} else {
		std::cout << "\nModels cleared\n";
	}
	return clear_result.status == howdy::native::UserModelStatus::kOk ? kClearExitOk
	                                                                  : kClearExitAbort;
}

auto ClearMain(const howdy::native::CommandInvocation &invocation) -> int {
	auto store = howdy::native::UserModelStore::Production();
	return howdy::native::clear_internal::ClearMainWithDependencies(
	    invocation, howdy::native::clear_internal::StoreDependencies(store));
}
