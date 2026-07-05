#include "storage/user_models.hpp"

#include "common/file_security.hpp"
#include "common/user_names.hpp"
#include "storage/user_model_codec.hpp"
#include "storage/user_model_limits.hpp"
#include "storage/user_model_store.hpp"

#include <algorithm>
#include <ctime>
#include <limits>
#include <string>
#include <utility>

namespace howdy::native {

	namespace {

		auto model_changed_failure() -> UserModelMutationResult {
			return UserModelMutationResult{
			    .status        = UserModelStatus::kModelChanged,
			    .error_message = "User model file changed, please rerun the command",
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
				return mutation_failure(UserModelStatus::kWriteFailed,
				                        "Failed to update model file");
			}
			if (user_model_codec::is_empty(*document)) {
				if (!transaction.remove_file()) {
					return mutation_failure(UserModelStatus::kDeleteFailed,
					                        "Failed to remove model file");
				}
				return UserModelMutationResult{
				    .status       = UserModelStatus::kOk,
				    .entry        = std::move(removed),
				    .removed_last = true,
				};
			}
			if (!transaction.write_document(*document)) {
				return mutation_failure(UserModelStatus::kWriteFailed,
				                        "Failed to update model file");
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
		const UserModelStore store;
		const auto document = store.load_document(user, expected_backend, expected_metric,
		                                          expected_model, true, default_secure_owner_uid());
		const auto &entries = document.result;
		if (entries.status != UserModelStatus::kOk) {
			return failure(entries.status, entries.error_message);
		}
		return entries;
	}

	auto inspect_user_model_file(const std::string &user) -> UserModelInspectResult {
		const UserModelStore store;
		return store.inspect(user);
	}

	auto append_user_model_entry(const std::string &user, const NewUserModelEntry &new_entry)
	    -> UserModelMutationResult {
		const UserModelStore store;
		auto                 mutation = store.begin_mutation(user);
		auto                &entries  = mutation.document.result;
		if (entries.status != UserModelStatus::kOk && entries.status != UserModelStatus::kNoModel) {
			return mutation_failure(entries.status, entries.error_message);
		}
		if (!mutation.transaction.has_value()) {
			return internal_invariant_failure();
		}
		if (!new_entry.label.empty() && !is_valid_model_label(new_entry.label)) {
			return mutation_failure(UserModelStatus::kInvalidShape,
			                        "New face model entry is invalid");
		}
		if (new_entry.encodings.empty()) {
			return mutation_failure(UserModelStatus::kInvalidShape,
			                        "New face model entry is invalid");
		}
		if (new_entry.encodings.size() > user_model_limits::kMaxEncodingsPerModel) {
			return mutation_failure(UserModelStatus::kOversized,
			                        "Stored face model contains too many encodings");
		}
		if (entries.entries.size() >= user_model_limits::kMaxStoredModels) {
			return mutation_failure(UserModelStatus::kOversized,
			                        "Stored face model list exceeds safety limit");
		}
		if (entries.next_id >= std::numeric_limits<int>::max()) {
			return mutation_failure(UserModelStatus::kInvalidShape,
			                        "Stored face model ID is too large");
		}
		for (const auto &entry : entries.entries) {
			if (!entry.backend.empty() && entry.backend != new_entry.backend) {
				return mutation_failure(UserModelStatus::kIncompatibleBackend,
				                        "Existing face models use incompatible face-recognition "
				                        "metadata");
			}
			if (!entry.metric.empty() && entry.metric != new_entry.metric) {
				return mutation_failure(UserModelStatus::kIncompatibleMetric,
				                        "Existing face models use incompatible face-recognition "
				                        "metadata");
			}
			if (!entry.model.empty() && entry.model != new_entry.model) {
				return mutation_failure(UserModelStatus::kIncompatibleModel,
				                        "Existing face models use incompatible face-recognition "
				                        "metadata");
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
		if (!user_model_codec::append_entry(mutation.document, entry) ||
		    !mutation.transaction->write_document(mutation.document)) {
			return mutation_failure(UserModelStatus::kWriteFailed, "Failed to save model file");
		}
		return UserModelMutationResult{.status = UserModelStatus::kOk, .entry = std::move(entry)};
	}

	auto remove_user_model_entry(const std::string &user, int id) -> UserModelMutationResult {
		const UserModelStore store;
		auto                 mutation = store.begin_mutation(user);
		auto                &entries  = mutation.document.result;
		if (entries.status != UserModelStatus::kOk) {
			return mutation_failure(entries.status, entries.error_message);
		}
		if (!mutation.transaction.has_value()) {
			return internal_invariant_failure();
		}

		const auto found = std::ranges::find_if(entries.entries, [id](const UserModelEntry &entry) {
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
		const UserModelStore store;
		auto                 mutation = store.begin_mutation(user);
		auto                &entries  = mutation.document.result;
		if (entries.status != UserModelStatus::kOk) {
			return mutation_failure(entries.status, entries.error_message);
		}
		if (!mutation.transaction.has_value()) {
			return internal_invariant_failure();
		}

		const auto found =
		    std::ranges::find_if(entries.entries, [&expected](const UserModelEntry &entry) {
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
		const UserModelStore store;
		auto                 transaction = store.lock_existing(user);
		if (transaction.status != UserModelStatus::kOk) {
			return mutation_failure(transaction.status, transaction.error_message);
		}
		if (!transaction.transaction.has_value()) {
			return internal_invariant_failure();
		}
		if (!transaction.transaction->remove_file()) {
			return mutation_failure(UserModelStatus::kDeleteFailed, "Failed to remove model file");
		}
		return UserModelMutationResult{
		    .status       = UserModelStatus::kOk,
		    .removed_last = true,
		};
	}

	auto clear_user_model_entries_if_unchanged(const std::string           &user,
	                                           const UserModelFileSnapshot &expected_snapshot)
	    -> UserModelMutationResult {
		const UserModelStore store;
		auto                 transaction = store.lock_existing(user);
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
			                        "Failed to inspect user model file: " +
			                            transaction.transaction->path().string());
		}
		if (!*unchanged) {
			return model_changed_failure();
		}
		if (!transaction.transaction->remove_file()) {
			return mutation_failure(UserModelStatus::kDeleteFailed, "Failed to remove model file");
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
		const UserModelStore store;
		const auto document = store.load_document(user, expected_backend, {}, {}, false, owner_uid);
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
