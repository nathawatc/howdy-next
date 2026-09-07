#pragma once

#include "storage/user_model_codec.hpp"
#include "storage/user_models.hpp"
#include "support/atomic_files.hpp"
#include "support/file_lock.hpp"

#include <filesystem>
#include <optional>
#include <string>

#include <sys/types.h>

namespace howdy::native {

	class UserModelStoreTransaction {
	public:
		UserModelStoreTransaction(const UserModelStoreTransaction &)                     = delete;
		auto operator=(const UserModelStoreTransaction &) -> UserModelStoreTransaction & = delete;

		UserModelStoreTransaction(UserModelStoreTransaction &&) noexcept = default;
		auto operator=(UserModelStoreTransaction &&) noexcept
		    -> UserModelStoreTransaction & = default;
		~UserModelStoreTransaction();

		[[nodiscard]] auto WriteDocument(const user_model_codec::Document &document) const
		    -> AtomicFileCommitResult;
		[[nodiscard]] auto RemoveFile() const -> AtomicFileCommitResult;

	private:
		std::filesystem::path path_;
		ScopedFileLock        namespace_lock_;
		ScopedFileLock        lock_;
		bool                  created_empty_file_ = false;
		mutable bool          completed_          = false;

		UserModelStoreTransaction(std::filesystem::path path, ScopedFileLock namespace_lock,
		                          ScopedFileLock lock, bool created_empty_file);

		[[nodiscard]] auto Path() const -> const std::filesystem::path &;
		[[nodiscard]] auto PathMatchesLockedFile() const -> bool;
		[[nodiscard]] auto Snapshot() const -> std::optional<UserModelFileSnapshot>;
		[[nodiscard]] auto SnapshotMatches(const UserModelFileSnapshot &expected) const
		    -> std::optional<bool>;

		friend class UserModelStore;
		friend auto ClearUserModelEntriesIfUnchanged(const std::string           &user,
		                                             const UserModelFileSnapshot &expected_snapshot)
		    -> UserModelMutationResult;
	};

	struct UserModelStoreMutationResult {
		std::optional<UserModelStoreTransaction> transaction;
		user_model_codec::Document               document;
	};

	struct UserModelStoreTransactionResult {
		UserModelStatus                          status = UserModelStatus::kOk;
		std::string                              error_message;
		std::optional<UserModelStoreTransaction> transaction;
	};

	class UserModelStore {
	public:
		UserModelStore() = default;

		[[nodiscard]] static auto BeginMutation(const std::string &user)
		    -> UserModelStoreMutationResult;
		[[nodiscard]] static auto LockExisting(const std::string &user)
		    -> UserModelStoreTransactionResult;

	private:
		struct UserModelExpectations {
			std::string               backend;
			std::optional<FaceMetric> metric;
			std::string               model;
			bool                      strict_shape;
		};

		struct UserModelPathResult {
			UserModelStatus       status = UserModelStatus::kOk;
			std::string           error_message;
			std::filesystem::path path;
		};

		[[nodiscard]] static auto Resolve(const std::string &user, bool create_directory,
		                                  std::optional<uid_t> owner_uid) -> UserModelPathResult;
		[[nodiscard]] static auto LoadDocument(const std::string           &user,
		                                       const UserModelExpectations &expectations,
		                                       std::optional<uid_t>         owner_uid)
		    -> user_model_codec::Document;
		[[nodiscard]] static auto Inspect(const std::string &user) -> UserModelInspectResult;
		[[nodiscard]] static auto LoadDocumentFromPath(const std::filesystem::path &path,
		                                               const UserModelExpectations &expectations)
		    -> user_model_codec::Document;
		[[nodiscard]] static auto LoadDocumentFromFd(int fd, const std::filesystem::path &path,
		                                             const UserModelExpectations &expectations,
		                                             bool treat_empty_as_no_model)
		    -> user_model_codec::Document;
		[[nodiscard]] static auto InspectRegularFileStatus(const std::filesystem::path &path,
		                                                   std::string *message) -> UserModelStatus;

		friend auto ListUserModelEntries(const std::string        &user,
		                                 const std::string        &expected_backend,
		                                 std::optional<FaceMetric> expected_metric,
		                                 const std::string &expected_model) -> UserModelListResult;
		friend auto InspectUserModelFile(const std::string &user) -> UserModelInspectResult;
		friend auto LoadUserModels(const std::string &user, const std::string &expected_backend,
		                           std::optional<uid_t> owner_uid) -> UserModelLoadResult;
	};

}  // namespace howdy::native
