#include "cli/remove.hpp"

#include "cli/remove/internal.hpp"
#include "storage/user_model_status.hpp"
#include "storage/user_models.hpp"

#include <charconv>
#include <iostream>
#include <string>
#include <system_error>

namespace {

	constexpr int kRemoveExitOk    = 0;
	constexpr int kRemoveExitAbort = 1;

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
	    std::from_chars(id_text.data(), id_text.data() + id_text.size(), id);
	if (parse_error != std::errc() || end != id_text.data() + id_text.size()) {
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
	    dependencies.remove_user_model_entry_if_matches(dependencies.context, user, expected);
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

auto RemoveMain(const howdy::native::CommandInvocation &invocation) -> int {
	return howdy::native::remove_internal::RemoveMainWithDependencies(
	    invocation,
	    howdy::native::remove_internal::RemoveDependencies{
	        .list_user_model_entries            = RemoveCliUserModelEntriesDependency,
	        .remove_user_model_entry_if_matches = RemoveUserModelEntryIfMatchesDependency,
	    });
}
