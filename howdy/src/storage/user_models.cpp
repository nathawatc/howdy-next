#include "storage/user_models.hpp"

#include "storage/user_model_codec.hpp"
#include "storage/user_model_limits.hpp"
#include "storage/user_model_status.hpp"
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

		auto ModelChangedFailure() -> UserModelMutationResult {
			return UserModelMutationResult{
			    .status        = UserModelStatus::kModelChanged,
			    .error_message = std::string(kUserModelChangedMessage),
			};
		}

		auto UserModelsListFailure(UserModelStatus status, std::string message)
		    -> UserModelListResult {
			return UserModelListResult{
			    .status        = status,
			    .error_message = std::move(message),
			};
		}

		auto MutationFailure(UserModelStatus status, std::string message)
		    -> UserModelMutationResult {
			return UserModelMutationResult{
			    .status        = status,
			    .error_message = std::move(message),
			};
		}

		auto InternalInvariantFailure() -> UserModelMutationResult {
			return MutationFailure(UserModelStatus::kParseError,
			                       "Internal user model store invariant failed");
		}

		auto CommitFailure(AtomicFileCommitResult result, UserModelStatus failure_status,
		                   std::string failure_message, std::string committed_message,
		                   UserModelEntry entry = {}, bool removed_last = false)
		    -> UserModelMutationResult {
			if (result == AtomicFileCommitResult::kAtomicExchangeUnsupported) {
				return MutationFailure(
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
			return MutationFailure(failure_status, std::move(failure_message));
		}

		auto EntryMatches(const UserModelEntry &entry, const UserModelEntryExpectation &expected)
		    -> bool {
			return entry.id == expected.id && entry.time == expected.time &&
			       entry.label == expected.label && entry.backend == expected.backend &&
			       entry.metric == expected.metric && entry.model == expected.model;
		}

		auto RemapLoadStatus(UserModelStatus status) -> UserModelStatus {
			if (status == UserModelStatus::kNoModelDirectory) {
				return UserModelStatus::kNoModel;
			}
			if (status == UserModelStatus::kOversized || status == UserModelStatus::kInvalidShape) {
				return UserModelStatus::kParseError;
			}
			return status;
		}

		auto RemoveEntryFromDocument(const UserModelStoreTransaction &transaction,
		                             user_model_codec::Document *document, UserModelEntry removed,
		                             std::vector<UserModelEntry>::difference_type found_index)
		    -> UserModelMutationResult {
			if (!user_model_codec::EraseEntry(*document, static_cast<std::size_t>(found_index))) {
				return MutationFailure(UserModelStatus::kWriteFailed, kModelUpdateFailedMessage);
			}
			if (user_model_codec::IsEmpty(*document)) {
				const auto commit_result = transaction.RemoveFile();
				if (!AtomicFileCommitIsDurable(commit_result)) {
					return CommitFailure(commit_result, UserModelStatus::kDeleteFailed,
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
			const auto commit_result = transaction.WriteDocument(*document);
			if (!AtomicFileCommitIsDurable(commit_result)) {
				return CommitFailure(
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

	auto ListUserModelEntries(const std::string &user, const std::string &expected_backend,
	                          std::optional<FaceMetric>                     expected_metric,
	                          const std::string                            &expected_model,
	                          const file_security_internal::ValidationRoot &validation_root)
	    -> UserModelListResult {
		const auto document =
		    UserModelStore::LoadDocument(user,
		                                 {.backend      = expected_backend,
		                                  .metric       = expected_metric,
		                                  .model        = expected_model,
		                                  .strict_shape = true},
		                                 DefaultSecureOwnerUid(), validation_root);
		const auto &entries = document.result;
		if (entries.status != UserModelStatus::kOk) {
			return UserModelsListFailure(entries.status, entries.error_message);
		}
		return entries;
	}

	auto InspectUserModelFile(const std::string                            &user,
	                          const file_security_internal::ValidationRoot &validation_root)
	    -> UserModelInspectResult {
		return UserModelStore::Inspect(user, validation_root);
	}

	auto AppendUserModelEntry(const std::string &user, const NewUserModelEntry &new_entry,
	                          const file_security_internal::ValidationRoot &validation_root)
	    -> UserModelMutationResult {
		auto  mutation = UserModelStore::BeginMutation(user, validation_root);
		auto &entries  = mutation.document.result;
		if (entries.status != UserModelStatus::kOk && entries.status != UserModelStatus::kNoModel) {
			return MutationFailure(entries.status, entries.error_message);
		}
		if (!mutation.transaction.has_value()) {
			return InternalInvariantFailure();
		}
		if (!new_entry.label.empty() && !IsValidModelLabel(new_entry.label)) {
			return MutationFailure(UserModelStatus::kInvalidShape,
			                       kNewFaceModelEntryInvalidMessage);
		}
		if (new_entry.encodings.empty()) {
			return MutationFailure(UserModelStatus::kInvalidShape,
			                       kNewFaceModelEntryInvalidMessage);
		}
		if (new_entry.encodings.size() > user_model_limits::kMaxEncodingsPerModel) {
			return MutationFailure(UserModelStatus::kOversized,
			                       std::string(kStoredEncodingsLimitMessage));
		}
		if (entries.entries.size() >= user_model_limits::kMaxStoredModels) {
			return MutationFailure(UserModelStatus::kOversized,
			                       std::string(kStoredModelListLimitMessage));
		}
		if (entries.next_id >= std::numeric_limits<int>::max()) {
			return MutationFailure(UserModelStatus::kInvalidShape,
			                       std::string(kStoredModelIdLimitMessage));
		}
		for (const auto &entry : entries.entries) {
			if (!entry.backend.empty() && entry.backend != new_entry.backend) {
				return MutationFailure(UserModelStatus::kIncompatibleBackend,
				                       kExistingModelsIncompatibleMessage);
			}
			if (entry.metric.has_value() && *entry.metric != new_entry.metric) {
				return MutationFailure(UserModelStatus::kIncompatibleMetric,
				                       kExistingModelsIncompatibleMessage);
			}
			if (!entry.model.empty() && entry.model != new_entry.model) {
				return MutationFailure(UserModelStatus::kIncompatibleModel,
				                       kExistingModelsIncompatibleMessage);
			}
		}
		for (const auto &encoding : new_entry.encodings) {
			const auto validation = user_model_codec::ValidateEncoding(encoding);
			if (validation.status != UserModelStatus::kOk) {
				return MutationFailure(validation.status, validation.error_message);
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
		if (!user_model_codec::AppendEntry(mutation.document, entry)) {
			return MutationFailure(UserModelStatus::kWriteFailed, kModelSaveFailedMessage);
		}
		const auto commit_result = mutation.transaction->WriteDocument(mutation.document);
		if (!AtomicFileCommitIsDurable(commit_result)) {
			return CommitFailure(commit_result, UserModelStatus::kWriteFailed,
			                     kModelSaveFailedMessage,
			                     "Model file was updated, but its directory could not be synced; "
			                     "verify state before "
			                     "retrying",
			                     entry);
		}
		return UserModelMutationResult{.status = UserModelStatus::kOk, .entry = std::move(entry)};
	}

	auto RemoveUserModelEntry(const std::string &user, int id,
	                          const file_security_internal::ValidationRoot &validation_root)
	    -> UserModelMutationResult {
		auto  mutation = UserModelStore::BeginMutation(user, validation_root);
		auto &entries  = mutation.document.result;
		if (entries.status != UserModelStatus::kOk) {
			return MutationFailure(entries.status, entries.error_message);
		}
		if (!mutation.transaction.has_value()) {
			return InternalInvariantFailure();
		}

		const auto found =
		    std::ranges::find_if(entries.entries, [id](const UserModelEntry &entry) -> bool {
			    return entry.id == id;
		    });
		if (found == entries.entries.end()) {
			return MutationFailure(UserModelStatus::kModelNotFound, "Model ID was not found");
		}

		UserModelEntry removed     = *found;
		const auto     found_index = std::distance(entries.entries.begin(), found);
		return RemoveEntryFromDocument(*mutation.transaction, &mutation.document,
		                               std::move(removed), found_index);
	}

	auto RemoveUserModelEntryIfMatches(
	    const std::string &user, const UserModelEntryExpectation &expected,
	    const file_security_internal::ValidationRoot &validation_root) -> UserModelMutationResult {
		auto  mutation = UserModelStore::BeginMutation(user, validation_root);
		auto &entries  = mutation.document.result;
		if (entries.status != UserModelStatus::kOk) {
			return MutationFailure(entries.status, entries.error_message);
		}
		if (!mutation.transaction.has_value()) {
			return InternalInvariantFailure();
		}

		const auto found =
		    std::ranges::find_if(entries.entries, [&expected](const UserModelEntry &entry) -> bool {
			    return entry.id == expected.id;
		    });
		if (found == entries.entries.end()) {
			return ModelChangedFailure();
		}
		if (!EntryMatches(*found, expected)) {
			return ModelChangedFailure();
		}

		UserModelEntry removed     = *found;
		const auto     found_index = std::distance(entries.entries.begin(), found);
		return RemoveEntryFromDocument(*mutation.transaction, &mutation.document,
		                               std::move(removed), found_index);
	}

	auto ClearUserModelEntries(const std::string                            &user,
	                           const file_security_internal::ValidationRoot &validation_root)
	    -> UserModelMutationResult {
		auto transaction = UserModelStore::LockExisting(user, validation_root);
		if (transaction.status != UserModelStatus::kOk) {
			return MutationFailure(transaction.status, transaction.error_message);
		}
		if (!transaction.transaction.has_value()) {
			return InternalInvariantFailure();
		}
		const auto commit_result = transaction.transaction->RemoveFile();
		if (!AtomicFileCommitIsDurable(commit_result)) {
			return CommitFailure(commit_result, UserModelStatus::kDeleteFailed,
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

	auto ClearUserModelEntriesIfUnchanged(
	    const std::string &user, const UserModelFileSnapshot &expected_snapshot,
	    const file_security_internal::ValidationRoot &validation_root) -> UserModelMutationResult {
		auto transaction = UserModelStore::LockExisting(user, validation_root);
		if (transaction.status == UserModelStatus::kNoModel ||
		    transaction.status == UserModelStatus::kNoModelDirectory) {
			return ModelChangedFailure();
		}
		if (transaction.status != UserModelStatus::kOk) {
			return MutationFailure(transaction.status, transaction.error_message);
		}
		if (!transaction.transaction.has_value()) {
			return InternalInvariantFailure();
		}
		const auto unchanged = transaction.transaction->SnapshotMatches(expected_snapshot);
		if (!unchanged.has_value()) {
			return MutationFailure(UserModelStatus::kParseError,
			                       std::string(kUserModelFileInspectionFailedMessage) + ": " +
			                           transaction.transaction->Path().string());
		}
		if (!*unchanged) {
			return ModelChangedFailure();
		}
		const auto commit_result = transaction.transaction->RemoveFile();
		if (!AtomicFileCommitIsDurable(commit_result)) {
			return CommitFailure(commit_result, UserModelStatus::kDeleteFailed,
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

	auto LoadUserModels(const std::string &user, const std::string &expected_backend)
	    -> UserModelLoadResult {
		return LoadUserModels(user, expected_backend, DefaultSecureOwnerUid());
	}

	auto LoadUserModels(const std::string &user, const std::string &expected_backend,
	                    std::optional<uid_t>                          owner_uid,
	                    const file_security_internal::ValidationRoot &validation_root)
	    -> UserModelLoadResult {
		const auto document = UserModelStore::LoadDocument(
		    user, {.backend = expected_backend, .metric = {}, .model = {}, .strict_shape = false},
		    owner_uid, validation_root);
		const auto         &entries = document.result;
		UserModelLoadResult result{
		    .status        = RemapLoadStatus(entries.status),
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
