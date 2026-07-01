#include "storage/user_models.hpp"

#include "common/atomic_files.hpp"
#include "common/fd_io.hpp"
#include "common/file_lock.hpp"
#include "common/file_security.hpp"
#include "common/user_names.hpp"
#include "config/runtime_paths.hpp"
#include "storage/user_model_codec.hpp"
#include "storage/user_model_limits.hpp"
#include "storage/user_model_readiness.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <utility>

#include <sys/stat.h>

namespace howdy::native {

	namespace {

		constexpr mode_t kUserModelsDirMode = S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP;
		constexpr mode_t kUserModelFileMode = S_IRUSR | S_IWUSR;

		struct ModelPathResult {
			UserModelStatus       status = UserModelStatus::kOk;
			std::string           error_message;
			std::filesystem::path path;
		};

		using ModelDocument = user_model_codec::Document;

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

		auto inspect_failure(UserModelStatus status, std::string message)
		    -> UserModelInspectResult {
			return UserModelInspectResult{
			    .status        = status,
			    .error_message = std::move(message),
			};
		}

		auto resolve_model_path(const std::string &user, bool create_directory,
		                        std::optional<uid_t> owner_uid) -> ModelPathResult {
			const auto models_dir = resolve_user_models_dir();
			if (!create_directory) {
				const auto readiness = check_user_model_readiness(models_dir, user, owner_uid);
				return ModelPathResult{
				    .status        = readiness.status,
				    .error_message = readiness.error_message,
				    .path          = readiness.path,
				};
			}

			const auto model_path = resolve_user_model_path(models_dir, user);
			if (!model_path) {
				return ModelPathResult{
				    .status        = UserModelStatus::kInvalidUser,
				    .error_message = kInvalidUserNameMessage,
				};
			}

			std::error_code ec;
			if (!std::filesystem::exists(models_dir, ec)) {
				if (ec) {
					return ModelPathResult{
					    .status = UserModelStatus::kParseError,
					    .error_message =
					        "Failed to inspect user models directory: " + models_dir.string(),
					};
				}
				std::filesystem::create_directories(models_dir, ec);
				if (ec || chmod(models_dir.c_str(), kUserModelsDirMode) != 0) {
					return ModelPathResult{
					    .status = UserModelStatus::kDirectoryCreateFailed,
					    .error_message =
					        "Failed to create secure user models directory: " + models_dir.string(),
					};
				}
			}

			const auto directory_security = check_secure_root_owned_directory_tree(
			    models_dir, "User models directory", owner_uid);
			if (!directory_security.ok) {
				return ModelPathResult{
				    .status        = UserModelStatus::kInsecurePath,
				    .error_message = directory_security.error_message,
				};
			}

			ec.clear();
			const auto model_exists = std::filesystem::exists(*model_path, ec);
			if (ec) {
				return ModelPathResult{
				    .status        = UserModelStatus::kParseError,
				    .error_message = "Failed to inspect user model file: " + model_path->string(),
				};
			}
			if (model_exists) {
				const auto file_security = check_secure_root_owned_file_with_directory(
				    *model_path, "User models directory", "User model file", owner_uid);
				if (!file_security.ok) {
					return ModelPathResult{
					    .status        = UserModelStatus::kInsecurePath,
					    .error_message = file_security.error_message,
					};
				}
			}

			return ModelPathResult{.path = *model_path};
		}

		auto snapshot_model_file(const std::filesystem::path &path)
		    -> std::optional<UserModelFileSnapshot> {
			struct stat st{};
			if (stat(path.c_str(), &st) != 0) {
				return std::nullopt;
			}
			return UserModelFileSnapshot{
			    .dev            = static_cast<std::uint64_t>(st.st_dev),
			    .inode          = static_cast<std::uint64_t>(st.st_ino),
			    .size           = static_cast<std::uintmax_t>(st.st_size),
			    .mtime_seconds  = static_cast<long long>(st.st_mtim.tv_sec),
			    .mtime_nanosecs = static_cast<long long>(st.st_mtim.tv_nsec),
			    .ctime_seconds  = static_cast<long long>(st.st_ctim.tv_sec),
			    .ctime_nanosecs = static_cast<long long>(st.st_ctim.tv_nsec),
			};
		}

		auto snapshots_match(const UserModelFileSnapshot &left, const UserModelFileSnapshot &right)
		    -> bool {
			return left.dev == right.dev && left.inode == right.inode && left.size == right.size &&
			       left.mtime_seconds == right.mtime_seconds &&
			       left.mtime_nanosecs == right.mtime_nanosecs &&
			       left.ctime_seconds == right.ctime_seconds &&
			       left.ctime_nanosecs == right.ctime_nanosecs;
		}

		auto entry_matches(const UserModelEntry &entry, const UserModelEntryExpectation &expected)
		    -> bool {
			return entry.id == expected.id && entry.time == expected.time &&
			       entry.label == expected.label && entry.backend == expected.backend &&
			       entry.metric == expected.metric && entry.model == expected.model;
		}

		auto inspect_regular_file_status(const std::filesystem::path &path, std::string *message)
		    -> UserModelStatus {
			std::error_code regular_ec;
			if (std::filesystem::is_regular_file(path, regular_ec)) {
				return UserModelStatus::kOk;
			}
			if (regular_ec) {
				std::error_code exists_ec;
				const auto      exists = std::filesystem::exists(path, exists_ec);
				if (!exists && !exists_ec) {
					return UserModelStatus::kNoModel;
				}
				*message = "Failed to inspect user model file: " + path.string();
				return UserModelStatus::kParseError;
			}
			return UserModelStatus::kNoModel;
		}

		auto opened_model_file_is_secure(const struct stat &opened_file) -> bool {
			const auto owner_uid = default_secure_owner_uid();
			return S_ISREG(opened_file.st_mode) &&
			       (!owner_uid.has_value() || opened_file.st_uid == *owner_uid) &&
			       (opened_file.st_mode & (S_IWGRP | S_IWOTH)) == 0 && opened_file.st_nlink == 1;
		}

		auto load_document_from_path(const std::filesystem::path &path,
		                             const std::string           &expected_backend,
		                             const std::string           &expected_metric,
		                             const std::string &expected_model, bool strict_shape = true)
		    -> ModelDocument {
			ScopedFd input(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
			if (input.get() < 0) {
				if (errno == ENOENT) {
					return ModelDocument(UserModelListResult{.status = UserModelStatus::kNoModel});
				}
				return ModelDocument{
				    failure(UserModelStatus::kParseError,
				            "Failed to open user model file: " + path.string()),
				};
			}
			struct stat opened_file{};
			if (fstat(input.get(), &opened_file) != 0) {
				return ModelDocument{
				    failure(UserModelStatus::kParseError,
				            "Failed to inspect opened user model file: " + path.string()),
				};
			}
			if (!opened_model_file_is_secure(opened_file)) {
				return ModelDocument{
				    failure(UserModelStatus::kInsecurePath,
				            "Opened user model file failed security validation: " + path.string()),
				};
			}
			if (opened_file.st_size < 0 ||
			    std::cmp_greater(opened_file.st_size, user_model_limits::kMaxUserModelFileBytes)) {
				return ModelDocument(
				    failure(UserModelStatus::kOversized,
				            "User model file is too large or unreadable: " + path.string()));
			}

			const auto content = read_fd_to_string_bounded(
			    input.get(), user_model_limits::kMaxUserModelFileBytes + 1);
			if (content.read_error) {
				return ModelDocument{
				    failure(UserModelStatus::kParseError,
				            "Failed to read user model file: " + path.string()),
				};
			}
			if (content.hit_limit) {
				return ModelDocument{
				    failure(UserModelStatus::kOversized,
				            "User model file is too large or unreadable: " + path.string()),
				};
			}

			return user_model_codec::decode_document(content.output, expected_backend,
			                                         expected_metric, expected_model, strict_shape);
		}

		auto load_entries_from_path(const std::filesystem::path &path,
		                            const std::string           &expected_backend,
		                            const std::string           &expected_metric,
		                            const std::string &expected_model, bool strict_shape = true)
		    -> UserModelListResult {
			return load_document_from_path(path, expected_backend, expected_metric, expected_model,
			                               strict_shape)
			    .result;
		}

		auto write_models(const std::filesystem::path &path, const ModelDocument &document)
		    -> bool {
			const auto serialized = user_model_codec::serialize_document(document);
			return serialized.has_value() &&
			       write_atomic_file(path, *serialized, kUserModelFileMode);
		}

		auto load_for_mutation(const std::string &user, ModelPathResult *path_result,
		                       std::optional<ScopedFileLock> *lock) -> ModelDocument {
			*path_result = resolve_model_path(user, true, default_secure_owner_uid());
			if (path_result->status != UserModelStatus::kOk) {
				return ModelDocument(failure(path_result->status, path_result->error_message));
			}

			*lock = acquire_file_lock(path_result->path);
			if (!lock->has_value()) {
				return ModelDocument(
				    failure(UserModelStatus::kLockFailed, "Failed to lock model file"));
			}

			const auto secured_path = resolve_model_path(user, true, default_secure_owner_uid());
			if (secured_path.status != UserModelStatus::kOk) {
				return ModelDocument(failure(secured_path.status, secured_path.error_message));
			}
			return load_document_from_path(path_result->path, {}, {}, {});
		}

		auto lock_existing_model_path(const std::string &user, ModelPathResult *path_result,
		                              std::optional<ScopedFileLock> *lock) -> UserModelStatus {
			*path_result = resolve_model_path(user, false, default_secure_owner_uid());
			if (path_result->status != UserModelStatus::kOk) {
				return path_result->status;
			}
			std::string regular_error;
			const auto  regular_status =
			    inspect_regular_file_status(path_result->path, &regular_error);
			if (regular_status != UserModelStatus::kOk) {
				path_result->error_message = regular_error;
				return regular_status;
			}

			*lock = acquire_file_lock(path_result->path);
			if (!lock->has_value()) {
				path_result->error_message = "Failed to lock model file";
				return UserModelStatus::kLockFailed;
			}

			const auto secured_path = resolve_model_path(user, false, default_secure_owner_uid());
			if (secured_path.status != UserModelStatus::kOk) {
				path_result->error_message = secured_path.error_message;
				return secured_path.status;
			}
			regular_error.clear();
			const auto secured_regular_status =
			    inspect_regular_file_status(path_result->path, &regular_error);
			if (secured_regular_status != UserModelStatus::kOk) {
				path_result->error_message = regular_error;
				return secured_regular_status;
			}
			return UserModelStatus::kOk;
		}

		auto remove_entry_from_document(const std::filesystem::path &path, ModelDocument *document,
		                                UserModelEntry                               removed,
		                                std::vector<UserModelEntry>::difference_type found_index)
		    -> UserModelMutationResult {
			if (!user_model_codec::erase_entry(*document, static_cast<std::size_t>(found_index))) {
				return mutation_failure(UserModelStatus::kWriteFailed,
				                        "Failed to update model file");
			}
			if (user_model_codec::is_empty(*document)) {
				if (!remove_file_and_sync(path)) {
					return mutation_failure(UserModelStatus::kDeleteFailed,
					                        "Failed to remove model file");
				}
				return UserModelMutationResult{
				    .status       = UserModelStatus::kOk,
				    .entry        = std::move(removed),
				    .removed_last = true,
				};
			}
			if (!write_models(path, *document)) {
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
		const auto path_result = resolve_model_path(user, false, default_secure_owner_uid());
		if (path_result.status != UserModelStatus::kOk) {
			return failure(path_result.status, path_result.error_message);
		}
		return load_entries_from_path(path_result.path, expected_backend, expected_metric,
		                              expected_model);
	}

	auto inspect_user_model_file(const std::string &user) -> UserModelInspectResult {
		const auto path_result = resolve_model_path(user, false, default_secure_owner_uid());
		if (path_result.status != UserModelStatus::kOk) {
			return inspect_failure(path_result.status, path_result.error_message);
		}
		std::string regular_error;
		const auto  regular_status = inspect_regular_file_status(path_result.path, &regular_error);
		if (regular_status != UserModelStatus::kOk) {
			return inspect_failure(regular_status, regular_error);
		}
		const auto snapshot = snapshot_model_file(path_result.path);
		if (!snapshot.has_value()) {
			return inspect_failure(UserModelStatus::kParseError,
			                       "Failed to inspect user model file: " +
			                           path_result.path.string());
		}
		return UserModelInspectResult{
		    .status   = UserModelStatus::kOk,
		    .snapshot = snapshot,
		};
	}

	auto append_user_model_entry(const std::string &user, const NewUserModelEntry &new_entry)
	    -> UserModelMutationResult {
		ModelPathResult               path_result;
		std::optional<ScopedFileLock> lock;
		auto                          document = load_for_mutation(user, &path_result, &lock);
		auto                         &entries  = document.result;
		if (entries.status != UserModelStatus::kOk && entries.status != UserModelStatus::kNoModel) {
			return mutation_failure(entries.status, entries.error_message);
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
		if (!user_model_codec::append_entry(document, entry) ||
		    !write_models(path_result.path, document)) {
			return mutation_failure(UserModelStatus::kWriteFailed, "Failed to save model file");
		}
		return UserModelMutationResult{.status = UserModelStatus::kOk, .entry = std::move(entry)};
	}

	auto remove_user_model_entry(const std::string &user, int id) -> UserModelMutationResult {
		ModelPathResult               path_result;
		std::optional<ScopedFileLock> lock;
		auto                          document = load_for_mutation(user, &path_result, &lock);
		auto                         &entries  = document.result;
		if (entries.status != UserModelStatus::kOk) {
			return mutation_failure(entries.status, entries.error_message);
		}

		const auto found = std::ranges::find_if(entries.entries, [id](const UserModelEntry &entry) {
			return entry.id == id;
		});
		if (found == entries.entries.end()) {
			return mutation_failure(UserModelStatus::kModelNotFound, "Model ID was not found");
		}

		UserModelEntry removed     = *found;
		const auto     found_index = std::distance(entries.entries.begin(), found);
		return remove_entry_from_document(path_result.path, &document, std::move(removed),
		                                  found_index);
	}

	auto remove_user_model_entry_if_matches(const std::string               &user,
	                                        const UserModelEntryExpectation &expected)
	    -> UserModelMutationResult {
		ModelPathResult               path_result;
		std::optional<ScopedFileLock> lock;
		auto                          document = load_for_mutation(user, &path_result, &lock);
		auto                         &entries  = document.result;
		if (entries.status != UserModelStatus::kOk) {
			return mutation_failure(entries.status, entries.error_message);
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
		return remove_entry_from_document(path_result.path, &document, std::move(removed),
		                                  found_index);
	}

	auto clear_user_model_entries(const std::string &user) -> UserModelMutationResult {
		ModelPathResult               path_result;
		std::optional<ScopedFileLock> lock;
		const auto                    status = lock_existing_model_path(user, &path_result, &lock);
		if (status != UserModelStatus::kOk) {
			return mutation_failure(status, path_result.error_message);
		}
		if (!remove_file_and_sync(path_result.path)) {
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
		ModelPathResult               path_result;
		std::optional<ScopedFileLock> lock;
		const auto                    status = lock_existing_model_path(user, &path_result, &lock);
		if (status == UserModelStatus::kNoModel || status == UserModelStatus::kNoModelDirectory) {
			return model_changed_failure();
		}
		if (status != UserModelStatus::kOk) {
			return mutation_failure(status, path_result.error_message);
		}
		const auto current_snapshot = snapshot_model_file(path_result.path);
		if (!current_snapshot.has_value()) {
			return mutation_failure(UserModelStatus::kParseError,
			                        "Failed to inspect user model file: " +
			                            path_result.path.string());
		}
		if (!snapshots_match(*current_snapshot, expected_snapshot)) {
			return model_changed_failure();
		}
		if (!remove_file_and_sync(path_result.path)) {
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
		const auto path_result = resolve_model_path(user, false, owner_uid);
		if (path_result.status != UserModelStatus::kOk) {
			return UserModelLoadResult{
			    .status        = path_result.status == UserModelStatus::kNoModelDirectory
			                         ? UserModelStatus::kNoModel
			                         : path_result.status,
			    .error_message = path_result.error_message,
			};
		}

		const auto entries =
		    load_entries_from_path(path_result.path, expected_backend, {}, {}, false);
		UserModelLoadResult result{
		    .status        = entries.status == UserModelStatus::kOversized ||
		                             entries.status == UserModelStatus::kInvalidShape
		                         ? UserModelStatus::kParseError
		                         : entries.status,
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
