#include "storage/user_models.hpp"

#include "storage/user_model_codec.hpp"
#include "storage/user_model_status.hpp"
#include "storage/user_model_types.hpp"
#include "support/atomic_files.hpp"
#include "support/file_security.hpp"
#include "support/file_security/validation_root.hpp"
#include "user_model_store/internal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <expected>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <sys/types.h>

namespace howdy::native {

	namespace {
		constexpr auto kModelUpdateFailedMessage = "Failed to update model file";
		constexpr auto kModelRemoveFailedMessage = "Failed to remove model file";
		constexpr auto kModelSaveFailedMessage   = "Failed to save model file";
		constexpr auto kExistingModelsIncompatibleMessage =
		    "Existing face models use incompatible face-recognition metadata";
		constexpr auto kStoredModelsIncompatibleMessage =
		    "Stored face models use incompatible face-recognition metadata";

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

		// The one backend/model compatibility rule. An empty expectation is not checked.
		auto CheckCompatibility(const std::vector<UserModelEntry> &entries,
		                        const std::string                 &expected_backend,
		                        const std::string &expected_model) -> UserModelStatus {
			for (const auto &entry : entries) {
				if (!expected_backend.empty() && entry.backend != expected_backend) {
					return UserModelStatus::kIncompatibleBackend;
				}
				if (!expected_model.empty() && entry.model != expected_model) {
					return UserModelStatus::kIncompatibleModel;
				}
			}
			return UserModelStatus::kOk;
		}

		// Applies the compatibility rule to a successful load.
		auto RequireCompatible(UserModelListResult models, const std::string &expected_backend,
		                       const std::string &expected_model) -> UserModelListResult {
			if (models.status != UserModelStatus::kOk) {
				return models;
			}
			const auto compatibility =
			    CheckCompatibility(models.entries, expected_backend, expected_model);
			if (compatibility != UserModelStatus::kOk) {
				return UserModelsListFailure(compatibility, kStoredModelsIncompatibleMessage);
			}
			return models;
		}

		// Encodes entries and commits them through the locked transaction; an entry the codec
		// rejects becomes the mutation failure.
		auto WriteEntries(const UserModelStoreTransaction   &transaction,
		                  const std::vector<UserModelEntry> &entries)
		    -> std::expected<AtomicFileCommitResult, UserModelMutationResult> {
			auto encoded = user_model_codec::Encode(entries);
			if (encoded.status != UserModelStatus::kOk) {
				return std::unexpected(
				    MutationFailure(encoded.status, std::move(encoded.error_message)));
			}
			return transaction.WriteContent(encoded.content);
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

		auto RemoveEntry(const UserModelStoreTransaction             &transaction,
		                 std::vector<UserModelEntry>                  entries,
		                 std::vector<UserModelEntry>::difference_type found_index)
		    -> UserModelMutationResult {
			UserModelEntry removed = std::move(entries.at(static_cast<std::size_t>(found_index)));
			entries.erase(entries.begin() + found_index);
			if (entries.empty()) {
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
			const auto commit_result = WriteEntries(transaction, entries);
			if (!commit_result.has_value()) {
				return commit_result.error();
			}
			if (!AtomicFileCommitIsDurable(*commit_result)) {
				return CommitFailure(
				    *commit_result, UserModelStatus::kWriteFailed, kModelUpdateFailedMessage,
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
	                          const std::string                            &expected_model,
	                          const file_security_internal::ValidationRoot &validation_root)
	    -> UserModelListResult {
		return RequireCompatible(
		    UserModelStore::LoadModels(user, DefaultSecureOwnerUid(), validation_root),
		    expected_backend, expected_model);
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
		auto &models   = mutation.models;
		if (models.status != UserModelStatus::kOk && models.status != UserModelStatus::kNoModel) {
			return MutationFailure(models.status, models.error_message);
		}
		if (!mutation.transaction.has_value()) {
			return InternalInvariantFailure();
		}
		const auto compatibility =
		    CheckCompatibility(models.entries, new_entry.backend, new_entry.model);
		if (compatibility != UserModelStatus::kOk) {
			return MutationFailure(compatibility, kExistingModelsIncompatibleMessage);
		}

		// Limits, labels, ids, and encodings are checked by the codec when the list is encoded.
		UserModelEntry entry{
		    .id        = models.next_id,
		    .time      = static_cast<std::int64_t>(std::time(nullptr)),
		    .label     = new_entry.label.empty() ? ("Model #" + std::to_string(models.next_id))
			                                     : new_entry.label,
		    .backend   = new_entry.backend,
		    .metric    = new_entry.metric,
		    .model     = new_entry.model,
		    .encodings = new_entry.encodings,
		};
		models.entries.push_back(entry);
		const auto commit_result = WriteEntries(*mutation.transaction, models.entries);
		if (!commit_result.has_value()) {
			return commit_result.error();
		}
		if (!AtomicFileCommitIsDurable(*commit_result)) {
			return CommitFailure(*commit_result, UserModelStatus::kWriteFailed,
			                     kModelSaveFailedMessage,
			                     "Model file was updated, but its directory could not be synced; "
			                     "verify state before "
			                     "retrying",
			                     entry);
		}
		return UserModelMutationResult{.status = UserModelStatus::kOk, .entry = std::move(entry)};
	}

	auto RemoveUserModelEntryIfMatches(
	    const std::string &user, const UserModelEntryExpectation &expected,
	    const file_security_internal::ValidationRoot &validation_root) -> UserModelMutationResult {
		auto  mutation = UserModelStore::BeginMutation(user, validation_root);
		auto &entries  = mutation.models;
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

		const auto found_index = std::distance(entries.entries.begin(), found);
		return RemoveEntry(*mutation.transaction, std::move(entries.entries), found_index);
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
		auto entries = RequireCompatible(
		    UserModelStore::LoadModels(user, owner_uid, validation_root), expected_backend, {});
		UserModelLoadResult result{
		    .status        = RemapLoadStatus(entries.status),
		    .error_message = entries.error_message,
		};
		if (entries.status != UserModelStatus::kOk) {
			return result;
		}
		std::size_t encoding_count = 0;
		for (const auto &entry : entries.entries) {
			encoding_count += entry.encodings.size();
		}
		result.stored.encodings.reserve(encoding_count);
		result.stored.models.reserve(encoding_count);
		for (auto &entry : entries.entries) {
			for (auto &encoding : entry.encodings) {
				result.stored.encodings.push_back(std::move(encoding));
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
