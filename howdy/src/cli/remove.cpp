#include "cli/remove.hpp"

#include "app/command_invocation.hpp"
#include "cli/confirmation_prompt.hpp"
#include "cli/remove/internal.hpp"
#include "storage/user_model_status.hpp"
#include "storage/user_model_store.hpp"
#include "storage/user_model_types.hpp"

#include <charconv>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>

namespace {

	constexpr int kRemoveExitOk    = 0;
	constexpr int kRemoveExitAbort = 1;

	auto ListRemovableUserModelEntriesFromStore(void *context, const std::string &user)
	    -> howdy::native::UserModelListResult {
		const auto *store = static_cast<const howdy::native::UserModelStore *>(context);
		if (store == nullptr) {
			return {
			    .status        = howdy::native::UserModelStatus::kParseError,
			    .error_message = "User model store is not initialized",
			};
		}
		return store->ListEntries(user, {});
	}

	auto RemoveUserModelEntryFromStore(void *context, const std::string &user,
	                                   const howdy::native::UserModelEntryExpectation &expected)
	    -> howdy::native::UserModelMutationResult {
		const auto *store = static_cast<const howdy::native::UserModelStore *>(context);
		if (store == nullptr) {
			return {
			    .status        = howdy::native::UserModelStatus::kWriteFailed,
			    .error_message = "User model store is not initialized",
			};
		}
		return store->RemoveEntryIfMatches(user, expected);
	}

}  // namespace

auto howdy::native::remove_internal::StoreDependencies(howdy::native::UserModelStore &store)
    -> RemoveDependencies {
	return {
	    .context                            = &store,
	    .list_user_model_entries            = ListRemovableUserModelEntriesFromStore,
	    .remove_user_model_entry_if_matches = RemoveUserModelEntryFromStore,
	};
}

auto howdy::native::remove_internal::RemoveMainWithDependencies(
    const howdy::native::CommandInvocation &invocation, const RemoveDependencies &dependencies)
    -> int {
	if (dependencies.list_user_model_entries == nullptr ||
	    dependencies.remove_user_model_entry_if_matches == nullptr) {
		return kRemoveExitAbort;
	}

	if (!invocation.resolved_user.has_value() || invocation.positionals.empty()) {
		return kRemoveExitAbort;
	}
	const auto &user    = *invocation.resolved_user;
	const auto &id_text = invocation.positionals.front();

	const auto models = dependencies.list_user_model_entries(dependencies.context, user);
	if (howdy::native::Classify(models.status) == howdy::native::UserModelOutcome::kAbsent) {
		std::cout << "No face models found. Please run:\n";
		std::cout << "\n\thowdy add\n\n";
		return kRemoveExitAbort;
	}
	if (models.status != howdy::native::UserModelStatus::kOk) {
		std::cout << models.error_message << "\n";
		return kRemoveExitAbort;
	}

	const std::string_view id_view = id_text;
	int                    id      = -1;
	const auto [end, parse_error]  = std::from_chars(id_view.begin(), id_view.end(), id);
	if (parse_error != std::errc() || end != id_view.end()) {
		id = -1;
	}
	bool                                     found = false;
	std::string                              found_label;
	howdy::native::UserModelEntryExpectation expected;
	for (const auto &model : models.entries) {
		if (model.id == id && std::to_string(model.id) == id_text) {
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
		std::cout << "No model with ID " << id_text << " exists for " << user << "\n";
		return kRemoveExitAbort;
	}

	if (!invocation.assume_yes) {
		std::cout << "Model \"" << found_label << "\" will be removed for " << user << ".\n";
		if (!howdy::native::cli_internal::ConfirmInteractiveAction()) {
			return kRemoveExitAbort;
		}
		std::cout << "\n";
	}

	const auto remove_result =
	    dependencies.remove_user_model_entry_if_matches(dependencies.context, user, expected);
	if (remove_result.status != howdy::native::UserModelStatus::kOk) {
		std::cout << remove_result.error_message << "\n";
	} else if (remove_result.removed_last) {
		std::cout << "Removed final face model; face verification disabled for this user\n";
	} else {
		std::cout << "Removed model " << remove_result.entry.id << "\n";
	}
	return remove_result.status == howdy::native::UserModelStatus::kOk ? kRemoveExitOk
	                                                                   : kRemoveExitAbort;
}

auto RemoveMain(const howdy::native::CommandInvocation &invocation) -> int {
	auto store = howdy::native::UserModelStore::Production();
	return howdy::native::remove_internal::RemoveMainWithDependencies(
	    invocation, howdy::native::remove_internal::StoreDependencies(store));
}
