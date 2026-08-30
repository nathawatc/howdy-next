#include "storage/user_models.hpp"

#include "storage/user_model_codec.hpp"
#include "storage/user_model_limits.hpp"
#include "storage/user_model_store.hpp"
#include "support/file_security.hpp"
#include "support/user_names.hpp"

#include <algorithm>
#include <ctime>
#include <limits>
#include <string>
#include <utility>

namespace howdy::native {

	namespace {
		constexpr auto kNewFaceModelEntryInvalidMessage = "New face model entry is invalid";
		constexpr auto kModelUpdateFailedMessage        = "Failed to update model file";
		constexpr auto kModelRemoveFailedMessage        = "Failed to remove model file";
		constexpr auto kModelSaveFailedMessage          = "Failed to save model file";
		constexpr auto kExistingModelsIncompatibleMessage =
		    "Existing face models use incompatible face-recognition metadata";

		auto model_changed_failure() -> UserModelMutationResult {
			return UserModelMutationResult{
			    .status        = UserModelStatus::kModelChanged,
			    .error_message = std::string(kUserModelChangedMessage),
			};
		}

		auto failure(UserModelStatus status, std::string message) -> UserModelListResult {
			return UserModelListResult{
			    .status        = status,
			    .error_message = std::move(message),
			};
		}

		auto mutation_failure(UserModelStatus status, std::string message)
		    -> UserModelMutationResult {
			return UserModelMutationResult{
			    .status        = status,
			    .error_message = std::move(message),
			};
		}

		auto internal_invariant_failure() -> UserModelMutationResult {
			return mutation_failure(UserModelStatus::kParseError,
			                        "Internal user model store invariant failed");
		}

		auto commit_failure(AtomicFileCommitResult result, UserModelStatus failure_status,
		                    std::string failure_message, std::string committed_message,
		                    UserModelEntry entry = {}, bool removed_last = false)
		    -> UserModelMutationResult {
			if (result == AtomicFileCommitResult::kAtomicExchangeUnsupported) {
				return mutation_failure(
				    UserModelStatus::kAtomicExchangeUnsupported,
				    "Cannot update user model file: filesystem or kernel does not support required "
				    "atomic model-file exchange; no changes were made");
			}
			if (result == AtomicFileCommitResult::kStateUncertain) {
				return UserModelMutationResult{
				    .status        = UserModelStatus::kCommitStateUncertain,
				    .error_message = "User model namespace changed during commit and recovery "
				                     "failed; inspect state "
				                     "before retrying",
				};
			}
			if (result == AtomicFileCommitResult::kCommittedSyncFailed) {
				return UserModelMutationResult{
				    .status        = UserModelStatus::kDurabilityUncertain,
				    .error_message = std::move(committed_message),
				    .entry         = std::move(entry),
				    .removed_last  = removed_last,
				};
			}
			return mutation_failure(failure_status, std::move(failure_message));
		}

		auto entry_matches(const UserModelEntry &entry, const UserModelEntryExpectation &expected)
		    -> bool {
			return entry.id == expected.id && entry.time == expected.time &&
			       entry.label == expected.label && entry.backend == expected.backend &&
			       entry.metric == expected.metric && entry.model == expected.model;
		}

		auto remap_load_status(UserModelStatus status) -> UserModelStatus {
			if (status == UserModelStatus::kNoModelDirectory) {
				return UserModelStatus::kNoModel;
			}
			if (status == UserModelStatus::kOversized || status == UserModelStatus::kInvalidShape) {
				return UserModelStatus::kParseError;
			}
			return status;
		}

		auto remove_entry_from_document(const UserModelStoreTransaction             &transaction,
		                                user_model_codec::Document                  *document,
		                                UserModelEntry                               removed,
		                                std::vector<UserModelEntry>::difference_type found_index)
		    -> UserModelMutationResult {
			if (!user_model_codec::erase_entry(*document, static_cast<std::size_t>(found_index))) {
				return mutation_failure(UserModelStatus::kWriteFailed, kModelUpdateFailedMessage);
			}
			if (user_model_codec::is_empty(*document)) {
				const auto commit_result = transaction.remove_file();
				if (!atomic_file_commit_is_durable(commit_result)) {
					return commit_failure(commit_result, UserModelStatus::kDeleteFailed,
					                      kModelRemoveFailedMessage,
					                      "Model file was removed, but its directory could not be "
					                      "synced; verify state "
					                      "before retrying",
					                      std::move(removed), true);
				}
				return UserModelMutationResult{
				    .status       = UserModelStatus::kOk,
				    .entry        = std::move(removed),
				    .removed_last = true,
				};
			}
			const auto commit_result = transaction.write_document(*document);
			if (!atomic_file_commit_is_durable(commit_result)) {
				return commit_failure(
				    commit_result, UserModelStatus::kWriteFailed, kModelUpdateFailedMessage,
				    "Model file was updated, but its directory could not be synced; verify state "
				    "before retrying",
				    std::move(removed));
			}
			return UserModelMutationResult{
			    .status = UserModelStatus::kOk,
			    .entry  = std::move(removed),
			};
		}

	}  // namespace

	auto list_user_model_entries(const std::string &user, const std::string &expected_backend,
	                             const std::string &expected_metric,
	                             const std::string &expected_model) -> UserModelListResult {
		const auto  document = UserModelStore::load_document(user,
		                                                     {.backend      = expected_backend,
		                                                      .metric       = expected_metric,
		                                                      .model        = expected_model,
		                                                      .strict_shape = true},
		                                                     default_secure_owner_uid());
		const auto &entries  = document.result;
		if (entries.status != UserModelStatus::kOk) {
			return failure(entries.status, entries.error_message);
		}
		return entries;
	}

	auto inspect_user_model_file(const std::string &user) -> UserModelInspectResult {
		return UserModelStore::inspect(user);
	}

	auto append_user_model_entry(const std::string &user, const NewUserModelEntry &new_entry)
	    -> UserModelMutationResult {
		auto  mutation = UserModelStore::begin_mutation(user);
		auto &entries  = mutation.document.result;
		if (entries.status != UserModelStatus::kOk && entries.status != UserModelStatus::kNoModel) {
			return mutation_failure(entries.status, entries.error_message);
		}
		if (!mutation.transaction.has_value()) {
			return internal_invariant_failure();
		}
		if (!new_entry.label.empty() && !is_valid_model_label(new_entry.label)) {
			return mutation_failure(UserModelStatus::kInvalidShape,
			                        kNewFaceModelEntryInvalidMessage);
		}
		if (new_entry.encodings.empty()) {
			return mutation_failure(UserModelStatus::kInvalidShape,
			                        kNewFaceModelEntryInvalidMessage);
		}
		if (new_entry.encodings.size() > user_model_limits::kMaxEncodingsPerModel) {
			return mutation_failure(UserModelStatus::kOversized,
			                        std::string(kStoredEncodingsLimitMessage));
		}
		if (entries.entries.size() >= user_model_limits::kMaxStoredModels) {
			return mutation_failure(UserModelStatus::kOversized,
			                        std::string(kStoredModelListLimitMessage));
		}
		if (entries.next_id >= std::numeric_limits<int>::max()) {
			return mutation_failure(UserModelStatus::kInvalidShape,
			                        std::string(kStoredModelIdLimitMessage));
		}
		for (const auto &entry : entries.entries) {
			if (!entry.backend.empty() && entry.backend != new_entry.backend) {
				return mutation_failure(UserModelStatus::kIncompatibleBackend,
				                        kExistingModelsIncompatibleMessage);
			}
			if (!entry.metric.empty() && entry.metric != new_entry.metric) {
				return mutation_failure(UserModelStatus::kIncompatibleMetric,
				                        kExistingModelsIncompatibleMessage);
			}
			if (!entry.model.empty() && entry.model != new_entry.model) {
				return mutation_failure(UserModelStatus::kIncompatibleModel,
				                        kExistingModelsIncompatibleMessage);
			}
		}
		for (const auto &encoding : new_entry.encodings) {
			const auto validation = user_model_codec::validate_encoding(encoding);
			if (validation.status != UserModelStatus::kOk) {
				return mutation_failure(validation.status, validation.error_message);
			}
		}

		UserModelEntry entry{
		    .id        = entries.next_id,
		    .time      = static_cast<long long>(std::time(nullptr)),
		    .label     = new_entry.label.empty() ? ("Model #" + std::to_string(entries.next_id))
		                                         : new_entry.label,
		    .backend   = new_entry.backend,
		    .metric    = new_entry.metric,
		    .model     = new_entry.model,
		    .encodings = new_entry.encodings,
		};
		if (!user_model_codec::append_entry(mutation.document, entry)) {
			return mutation_failure(UserModelStatus::kWriteFailed, kModelSaveFailedMessage);
		}
		const auto commit_result = mutation.transaction->write_document(mutation.document);
		if (!atomic_file_commit_is_durable(commit_result)) {
			return commit_failure(commit_result, UserModelStatus::kWriteFailed,
			                      kModelSaveFailedMessage,
			                      "Model file was updated, but its directory could not be synced; "
			                      "verify state before "
			                      "retrying",
			                      entry);
		}
		return UserModelMutationResult{.status = UserModelStatus::kOk, .entry = std::move(entry)};
	}

	auto remove_user_model_entry(const std::string &user, int id) -> UserModelMutationResult {
		auto  mutation = UserModelStore::begin_mutation(user);
		auto &entries  = mutation.document.result;
		if (entries.status != UserModelStatus::kOk) {
			return mutation_failure(entries.status, entries.error_message);
		}
		if (!mutation.transaction.has_value()) {
			return internal_invariant_failure();
		}

		const auto found =
		    std::ranges::find_if(entries.entries, [id](const UserModelEntry &entry) -> bool {
			    return entry.id == id;
		    });
		if (found == entries.entries.end()) {
			return mutation_failure(UserModelStatus::kModelNotFound, "Model ID was not found");
		}

		UserModelEntry removed     = *found;
		const auto     found_index = std::distance(entries.entries.begin(), found);
		return remove_entry_from_document(*mutation.transaction, &mutation.document,
		                                  std::move(removed), found_index);
	}

	auto remove_user_model_entry_if_matches(const std::string               &user,
	                                        const UserModelEntryExpectation &expected)
	    -> UserModelMutationResult {
		auto  mutation = UserModelStore::begin_mutation(user);
		auto &entries  = mutation.document.result;
		if (entries.status != UserModelStatus::kOk) {
			return mutation_failure(entries.status, entries.error_message);
		}
		if (!mutation.transaction.has_value()) {
			return internal_invariant_failure();
		}

		const auto found =
		    std::ranges::find_if(entries.entries, [&expected](const UserModelEntry &entry) -> bool {
			    return entry.id == expected.id;
		    });
		if (found == entries.entries.end()) {
			return model_changed_failure();
		}
		if (!entry_matches(*found, expected)) {
			return model_changed_failure();
		}

		UserModelEntry removed     = *found;
		const auto     found_index = std::distance(entries.entries.begin(), found);
		return remove_entry_from_document(*mutation.transaction, &mutation.document,
		                                  std::move(removed), found_index);
	}

	auto clear_user_model_entries(const std::string &user) -> UserModelMutationResult {
		auto transaction = UserModelStore::lock_existing(user);
		if (transaction.status != UserModelStatus::kOk) {
			return mutation_failure(transaction.status, transaction.error_message);
		}
		if (!transaction.transaction.has_value()) {
			return internal_invariant_failure();
		}
		const auto commit_result = transaction.transaction->remove_file();
		if (!atomic_file_commit_is_durable(commit_result)) {
			return commit_failure(commit_result, UserModelStatus::kDeleteFailed,
			                      kModelRemoveFailedMessage,
			                      "Model file was removed, but its directory could not be synced; "
			                      "verify state before "
			                      "retrying",
			                      {}, true);
		}
		return UserModelMutationResult{
		    .status       = UserModelStatus::kOk,
		    .removed_last = true,
		};
	}

	auto clear_user_model_entries_if_unchanged(const std::string           &user,
	                                           const UserModelFileSnapshot &expected_snapshot)
	    -> UserModelMutationResult {
		auto transaction = UserModelStore::lock_existing(user);
		if (transaction.status == UserModelStatus::kNoModel ||
		    transaction.status == UserModelStatus::kNoModelDirectory) {
			return model_changed_failure();
		}
		if (transaction.status != UserModelStatus::kOk) {
			return mutation_failure(transaction.status, transaction.error_message);
		}
		if (!transaction.transaction.has_value()) {
			return internal_invariant_failure();
		}
		const auto unchanged = transaction.transaction->snapshot_matches(expected_snapshot);
		if (!unchanged.has_value()) {
			return mutation_failure(UserModelStatus::kParseError,
			                        std::string(kUserModelFileInspectionFailedMessage) + ": " +
			                            transaction.transaction->path().string());
		}
		if (!*unchanged) {
			return model_changed_failure();
		}
		const auto commit_result = transaction.transaction->remove_file();
		if (!atomic_file_commit_is_durable(commit_result)) {
			return commit_failure(commit_result, UserModelStatus::kDeleteFailed,
			                      kModelRemoveFailedMessage,
			                      "Model file was removed, but its directory could not be synced; "
			                      "verify state before "
			                      "retrying",
			                      {}, true);
		}
		return UserModelMutationResult{
		    .status       = UserModelStatus::kOk,
		    .removed_last = true,
		};
	}

	auto load_user_models(const std::string &user, const std::string &expected_backend)
	    -> UserModelLoadResult {
		return load_user_models(user, expected_backend, default_secure_owner_uid());
	}

	auto load_user_models(const std::string &user, const std::string &expected_backend,
	                      std::optional<uid_t> owner_uid) -> UserModelLoadResult {
		const auto document = UserModelStore::load_document(
		    user, {.backend = expected_backend, .metric = {}, .model = {}, .strict_shape = false},
		    owner_uid);
		const auto         &entries = document.result;
		UserModelLoadResult result{
		    .status        = remap_load_status(entries.status),
		    .error_message = entries.error_message,
		};
		if (entries.status != UserModelStatus::kOk) {
			return result;
		}
		for (const auto &entry : entries.entries) {
			for (const auto &encoding : entry.encodings) {
				result.stored.encodings.push_back(encoding);
				result.stored.models.push_back(EncodingModelInfo{
				    .id    = entry.id,
				    .label = entry.label,
				});
			}
		}
		result.status =
		    result.stored.encodings.empty() ? UserModelStatus::kNoModel : UserModelStatus::kOk;
		return result;
	}

}  // namespace howdy::native
