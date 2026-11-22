#include "../user_model_codec/internal.hpp"
#include "internal.hpp"
#include "storage/user_model_label.hpp"
#include "storage/user_model_limits.hpp"
#include "storage/user_model_status.hpp"
#include "storage/user_model_store.hpp"
#include "storage/user_model_types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace howdy::native::user_model_store_internal {

	namespace {

		constexpr auto kNewFaceModelEntryInvalidMessage = "New face model entry is invalid";
		constexpr auto kModelUpdateFailedMessage        = "Failed to update model file";
		constexpr auto kModelRemoveFailedMessage        = "Failed to remove model file";
		constexpr auto kModelSaveFailedMessage          = "Failed to save model file";
		constexpr auto kExistingModelsIncompatibleMessage =
		    "Existing face models use incompatible face-recognition metadata";
		constexpr auto kModelRemovedSyncFailedMessage =
		    "Model file was removed, but its directory could not be synced; verify state before "
		    "retrying";
		constexpr auto kModelUpdatedSyncFailedMessage =
		    "Model file was updated, but its directory could not be synced; verify state before "
		    "retrying";

		auto SnapshotsMatch(const UserModelFileSnapshot &left, const UserModelFileSnapshot &right)
		    -> bool {
			return left.dev == right.dev && left.inode == right.inode && left.size == right.size &&
			       left.mtime_seconds == right.mtime_seconds &&
			       left.mtime_nanosecs == right.mtime_nanosecs &&
			       left.ctime_seconds == right.ctime_seconds &&
			       left.ctime_nanosecs == right.ctime_nanosecs;
		}

		auto EntryMatches(const UserModelEntry &entry, const UserModelEntryExpectation &expected)
		    -> bool {
			return entry.id == expected.id && entry.time == expected.time &&
			       entry.label == expected.label && entry.backend == expected.backend &&
			       entry.metric == expected.metric && entry.model == expected.model;
		}

		auto AbortMutation(UserModelStatus status, std::string message) -> MutationDecision {
			return MutationDecision{
			    .action        = MutationAction::kAbort,
			    .status        = status,
			    .error_message = std::move(message),
			};
		}

		auto ValidateNewEntryForAppend(const UserModelListResult &entries,
		                               const NewUserModelEntry   &new_entry)
		    -> std::optional<MutationDecision> {
			if ((!new_entry.label.empty() && !IsValidModelLabel(new_entry.label)) ||
			    new_entry.encodings.empty()) {
				return AbortMutation(UserModelStatus::kInvalidShape,
				                     kNewFaceModelEntryInvalidMessage);
			}
			if (new_entry.encodings.size() > user_model_limits::kMaxEncodingsPerModel) {
				return AbortMutation(UserModelStatus::kOversized,
				                     std::string(kStoredEncodingsLimitMessage));
			}
			if (entries.entries.size() >= user_model_limits::kMaxStoredModels) {
				return AbortMutation(UserModelStatus::kOversized,
				                     std::string(kStoredModelListLimitMessage));
			}
			if (entries.next_id >= std::numeric_limits<int>::max()) {
				return AbortMutation(UserModelStatus::kInvalidShape,
				                     std::string(kStoredModelIdLimitMessage));
			}
			for (const auto &entry : entries.entries) {
				if (!entry.backend.empty() && entry.backend != new_entry.backend) {
					return AbortMutation(UserModelStatus::kIncompatibleBackend,
					                     kExistingModelsIncompatibleMessage);
				}
				if (!entry.model.empty() && entry.model != new_entry.model) {
					return AbortMutation(UserModelStatus::kIncompatibleModel,
					                     kExistingModelsIncompatibleMessage);
				}
			}
			for (const auto &encoding : new_entry.encodings) {
				const auto validation = user_model_codec::ValidateEncoding(encoding);
				if (validation.status != UserModelStatus::kOk) {
					return AbortMutation(validation.status, validation.error_message);
				}
			}
			return std::nullopt;
		}

	}  // namespace

	auto AppendEntry(const UserModelStoreConfig &config, const std::string &user,
	                 const NewUserModelEntry &new_entry) -> UserModelMutationResult {
		return Mutate(
		    config, user, {.existence = MutationExistence::kAllowMissing, .load_document = true},
		    [&](MutationContext &context) -> MutationDecision {
			    const auto &entries = context.document.Result();
			    if (auto failure = ValidateNewEntryForAppend(entries, new_entry)) {
				    return *failure;
			    }

			    UserModelEntry entry{
			        .id    = entries.next_id,
			        .time  = static_cast<std::int64_t>(std::time(nullptr)),
			        .label = new_entry.label.empty() ? ("Model #" + std::to_string(entries.next_id))
					                                 : new_entry.label,
			        .backend   = new_entry.backend,
			        .metric    = new_entry.metric,
			        .model     = new_entry.model,
			        .encodings = new_entry.encodings,
			    };
			    if (!user_model_codec::AppendEntry(context.document, entry)) {
				    return AbortMutation(UserModelStatus::kWriteFailed, kModelSaveFailedMessage);
			    }
			    return MutationDecision{
			        .action              = MutationAction::kWrite,
			        .failure_message     = kModelSaveFailedMessage,
			        .sync_failed_message = kModelUpdatedSyncFailedMessage,
			        .entry               = std::move(entry),
			    };
		    });
	}

	auto RemoveEntryIfMatches(const UserModelStoreConfig &config, const std::string &user,
	                          const UserModelEntryExpectation &expected)
	    -> UserModelMutationResult {
		return Mutate(
		    config, user, {.existence = MutationExistence::kRequireExisting, .load_document = true},
		    [&](MutationContext &context) -> MutationDecision {
			    auto      &entries = context.document.Result();
			    const auto found   = std::ranges::find_if(
			        entries.entries, [&expected](const UserModelEntry &entry) -> bool {
				        return entry.id == expected.id;
			        });
			    if (found == entries.entries.end() || !EntryMatches(*found, expected)) {
				    return AbortMutation(UserModelStatus::kModelChanged,
					                     std::string(kUserModelChangedMessage));
			    }

			    UserModelEntry removed     = *found;
			    const auto     found_index = std::distance(entries.entries.begin(), found);
			    if (!user_model_codec::EraseEntry(context.document,
				                                  static_cast<std::size_t>(found_index))) {
				    return AbortMutation(UserModelStatus::kWriteFailed, kModelUpdateFailedMessage);
			    }
			    if (user_model_codec::IsEmpty(context.document)) {
				    return MutationDecision{
				        .action              = MutationAction::kRemove,
				        .failure_message     = kModelRemoveFailedMessage,
				        .sync_failed_message = kModelRemovedSyncFailedMessage,
				        .entry               = std::move(removed),
				    };
			    }
			    return MutationDecision{
			        .action              = MutationAction::kWrite,
			        .failure_message     = kModelUpdateFailedMessage,
			        .sync_failed_message = kModelUpdatedSyncFailedMessage,
			        .entry               = std::move(removed),
			    };
		    });
	}

	auto ClearEntriesIfUnchanged(const UserModelStoreConfig &config, const std::string &user,
	                             const UserModelFileSnapshot &expected_snapshot)
	    -> UserModelMutationResult {
		return Mutate(config, user,
		              {.existence = MutationExistence::kRequireExisting, .load_document = false},
		              [&](MutationContext &context) -> MutationDecision {
			              if (!context.snapshot.has_value() ||
						      !SnapshotsMatch(*context.snapshot, expected_snapshot)) {
				              return AbortMutation(UserModelStatus::kModelChanged,
							                       std::string(kUserModelChangedMessage));
			              }
			              return MutationDecision{
			                  .action              = MutationAction::kRemove,
			                  .failure_message     = kModelRemoveFailedMessage,
			                  .sync_failed_message = kModelRemovedSyncFailedMessage,
			              };
		              });
	}

}  // namespace howdy::native::user_model_store_internal
