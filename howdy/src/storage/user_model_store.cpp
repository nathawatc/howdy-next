#include "storage/user_model_store.hpp"

#include "config/runtime_paths.hpp"
#include "storage/user_model_limits.hpp"
#include "storage/user_model_status.hpp"
#include "storage/user_model_types.hpp"
#include "support/fd_io.hpp"
#include "support/scoped_fd.hpp"
#include "user_model_codec/internal.hpp"
#include "user_model_store/internal.hpp"
#include "user_model_store/readiness/internal.hpp"

#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>

#include <fcntl.h>

#include <sys/types.h>

namespace howdy::native {

	namespace {

		using user_model_store_internal::UserModelExpectations;

		constexpr auto kUserModelFileTooLargeMessage =
		    "User model file is too large or unreadable: ";
		constexpr auto kUserModelFileReadFailedMessage = "Failed to read user model file: ";

		auto StoreListFailure(UserModelStatus status, std::string message) -> UserModelListResult {
			return UserModelListResult{
			    .status        = status,
			    .error_message = std::move(message),
			};
		}

		auto InspectFailure(UserModelStatus status, std::string message) -> UserModelInspectResult {
			return UserModelInspectResult{
			    .status        = status,
			    .error_message = std::move(message),
			};
		}

		auto LoadDocumentFromPath(const std::filesystem::path &path,
		                          const UserModelExpectations &expectations, uid_t owner_uid)
		    -> user_model_codec::Document {
			ScopedFd const input(
			    open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
			if (input.Get() < 0) {
				const int error_number = errno;
				if (error_number == ENOENT) {
					return user_model_codec::Document(
					    UserModelListResult{.status = UserModelStatus::kNoModel});
				}
				if (user_model_store_internal::OpenErrorMeansNotRegularFile(error_number)) {
					return user_model_codec::Document{
					    StoreListFailure(UserModelStatus::kInsecurePath,
						                 user_model_store_internal::NotRegularFileMessage(
					                         kUserModelFileLabel, path)),
					};
				}
				return user_model_codec::Document{
				    StoreListFailure(
				        UserModelStatus::kParseError,
				        user_model_store_internal::WithErrno(
				            "Failed to open user model file: " + path.string(), error_number)),
				};
			}
			return user_model_store_internal::LoadDocumentFromFd(input.Get(), path, expectations,
			                                                     owner_uid);
		}

		auto LoadDocument(const UserModelStoreConfig &config, const std::string &user,
		                  const UserModelExpectations &expectations) -> user_model_codec::Document {
			const auto path_result = user_model_store_internal::CheckReadiness(config, user);
			if (path_result.status != UserModelStatus::kOk) {
				return user_model_codec::Document(
				    StoreListFailure(path_result.status, path_result.error_message));
			}
			if (config.hooks.before_read_open) {
				config.hooks.before_read_open(path_result.path);
			}
			return LoadDocumentFromPath(path_result.path, expectations, config.owner_uid);
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

	}  // namespace

	namespace user_model_store_internal {

		auto LoadDocumentFromFd(int fd, const std::filesystem::path &path,
		                        const UserModelExpectations &expectations, uid_t owner_uid)
		    -> user_model_codec::Document {
			const auto validated =
			    ValidateOpenedModelFd(fd, path, owner_uid, EmptyModelPolicy::kReject);
			if (validated.status != UserModelStatus::kOk) {
				return user_model_codec::Document{
				    StoreListFailure(validated.status, validated.error_message),
				};
			}
			const auto &opened_file = validated.file_stat;
			if (opened_file.st_size < 0 ||
			    std::cmp_greater(opened_file.st_size, user_model_limits::kMaxUserModelFileBytes)) {
				return user_model_codec::Document(
				    StoreListFailure(UserModelStatus::kOversized,
					                 std::string(kUserModelFileTooLargeMessage) + path.string()));
			}
			// Read positionally from the start: the offset of an inherited descriptor is shared.
			const auto content = ReadFdToStringBounded({
			    .fd            = fd,
			    .max_bytes     = user_model_limits::kMaxUserModelFileBytes + 1,
			    .reserve_bytes = static_cast<std::size_t>(opened_file.st_size),
			    .offset        = off_t{0},
			});
			if (content.read_error) {
				return user_model_codec::Document{
				    StoreListFailure(UserModelStatus::kParseError,
					                 std::string(kUserModelFileReadFailedMessage) + path.string()),
				};
			}
			if (content.hit_limit) {
				return user_model_codec::Document{
				    StoreListFailure(UserModelStatus::kOversized,
					                 std::string(kUserModelFileTooLargeMessage) + path.string()),
				};
			}

			return user_model_codec::DecodeDocument(content.output, expectations.backend,
			                                        expectations.model, expectations.strict_shape);
		}

		auto FlattenLoadedModels(user_model_codec::Document &document) -> UserModelLoadResult {
			auto               &entries = document.Result();
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

	}  // namespace user_model_store_internal

	auto UserModelStore::Production() -> UserModelStore {
		return UserModelStore({
		    .models_dir = ResolveUserModelsDir(),
		    .owner_uid  = 0,
		});
	}

	auto UserModelStore::ListEntries(const std::string &user, const std::string &expected_backend,
	                                 const std::string &expected_model) const
	    -> UserModelListResult {
		const auto  document = LoadDocument(config_, user,
		                                    {
		                                        .backend      = expected_backend,
		                                        .model        = expected_model,
		                                        .strict_shape = true,
		                                    });
		const auto &entries  = document.Result();
		if (entries.status != UserModelStatus::kOk) {
			return user_model_codec_internal::UserModelListFailure(entries.status,
			                                                       entries.error_message);
		}
		return entries;
	}

	auto UserModelStore::InspectFile(const std::string &user) const -> UserModelInspectResult {
		const auto path_result = user_model_store_internal::CheckReadiness(config_, user);
		if (path_result.status != UserModelStatus::kOk) {
			return InspectFailure(path_result.status, path_result.error_message);
		}
		if (config_.hooks.before_read_open) {
			config_.hooks.before_read_open(path_result.path);
		}
		ScopedFd const input(
		    open(path_result.path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
		if (!input.Valid()) {
			const int error_number = errno;
			if (error_number == ENOENT) {
				return InspectFailure(UserModelStatus::kNoModel, {});
			}
			if (user_model_store_internal::OpenErrorMeansNotRegularFile(error_number)) {
				return InspectFailure(UserModelStatus::kInsecurePath,
				                      user_model_store_internal::NotRegularFileMessage(
				                          kUserModelFileLabel, path_result.path));
			}
			return InspectFailure(UserModelStatus::kParseError,
			                      user_model_store_internal::UserModelInspectionFailedMessage(
			                          path_result.path, error_number));
		}
		auto validated = user_model_store_internal::ValidateOpenedModelFd(
		    input.Get(), path_result.path, config_.owner_uid,
		    user_model_store_internal::EmptyModelPolicy::kAccept);
		if (validated.status != UserModelStatus::kOk) {
			return InspectFailure(validated.status, std::move(validated.error_message));
		}
		return UserModelInspectResult{
		    .status   = UserModelStatus::kOk,
		    .snapshot = user_model_store_internal::SnapshotFromStat(validated.file_stat),
		};
	}

	auto UserModelStore::LoadModels(const std::string &user, const std::string &expected_backend,
	                                const std::string &expected_model) const
	    -> UserModelLoadResult {
		auto document = LoadDocument(
		    config_, user,
		    {.backend = expected_backend, .model = expected_model, .strict_shape = false});
		return user_model_store_internal::FlattenLoadedModels(document);
	}

	auto UserModelStore::AppendEntry(const std::string       &user,
	                                 const NewUserModelEntry &new_entry) const
	    -> UserModelMutationResult {
		return user_model_store_internal::AppendEntry(config_, user, new_entry);
	}

	auto UserModelStore::RemoveEntryIfMatches(const std::string               &user,
	                                          const UserModelEntryExpectation &expected) const
	    -> UserModelMutationResult {
		return user_model_store_internal::RemoveEntryIfMatches(config_, user, expected);
	}

	auto
	UserModelStore::ClearEntriesIfUnchanged(const std::string           &user,
	                                        const UserModelFileSnapshot &expected_snapshot) const
	    -> UserModelMutationResult {
		return user_model_store_internal::ClearEntriesIfUnchanged(config_, user, expected_snapshot);
	}

}  // namespace howdy::native
