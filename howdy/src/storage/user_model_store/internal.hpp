#pragma once

#include "storage/user_model_types.hpp"
#include "storage/user_models.hpp"
#include "support/atomic_files.hpp"
#include "support/file_lock.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

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

		// Commits already-encoded model file bytes.
		[[nodiscard]] auto WriteContent(std::string_view content) const -> AtomicFileCommitResult;
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
		friend auto ClearUserModelEntriesIfUnchanged(
		    const std::string &user, const UserModelFileSnapshot &expected_snapshot,
		    const file_security_internal::ValidationRoot &validation_root)
		    -> UserModelMutationResult;
	};

	struct UserModelStoreMutationResult {
		std::optional<UserModelStoreTransaction> transaction;
		UserModelListResult                      models;
	};

	struct UserModelStoreTransactionResult {
		UserModelStatus                          status = UserModelStatus::kOk;
		std::string                              error_message;
		std::optional<UserModelStoreTransaction> transaction;
	};

	class UserModelStore {
	public:
		UserModelStore() = delete;

		[[nodiscard]] static auto
		BeginMutation(const std::string                            &user,
		              const file_security_internal::ValidationRoot &validation_root = {})
		    -> UserModelStoreMutationResult;
		[[nodiscard]] static auto
		LockExisting(const std::string                            &user,
		             const file_security_internal::ValidationRoot &validation_root = {})
		    -> UserModelStoreTransactionResult;

	private:
		struct UserModelPathResult {
			UserModelStatus       status = UserModelStatus::kOk;
			std::string           error_message;
			std::filesystem::path path;
		};

		[[nodiscard]] static auto
		Resolve(const std::string &user, bool create_directory, std::optional<uid_t> owner_uid,
		        const file_security_internal::ValidationRoot &validation_root)
		    -> UserModelPathResult;
		[[nodiscard]] static auto
		LoadModels(const std::string &user, std::optional<uid_t> owner_uid,
		           const file_security_internal::ValidationRoot &validation_root)
		    -> UserModelListResult;
		[[nodiscard]] static auto
		Inspect(const std::string                            &user,
		        const file_security_internal::ValidationRoot &validation_root)
		    -> UserModelInspectResult;
		[[nodiscard]] static auto LoadModelsFromPath(const std::filesystem::path &path)
		    -> UserModelListResult;
		[[nodiscard]] static auto LoadModelsFromFd(int fd, const std::filesystem::path &path,
		                                           bool treat_empty_as_no_model)
		    -> UserModelListResult;
		[[nodiscard]] static auto InspectRegularFileStatus(const std::filesystem::path &path,
		                                                   std::string *message) -> UserModelStatus;

		friend auto
		ListUserModelEntries(const std::string &user, const std::string &expected_backend,
		                     const std::string                            &expected_model,
		                     const file_security_internal::ValidationRoot &validation_root)
		    -> UserModelListResult;
		friend auto
		InspectUserModelFile(const std::string                            &user,
		                     const file_security_internal::ValidationRoot &validation_root)
		    -> UserModelInspectResult;
		friend auto LoadUserModels(const std::string &user, const std::string &expected_backend,
		                           std::optional<uid_t>                          owner_uid,
		                           const file_security_internal::ValidationRoot &validation_root)
		    -> UserModelLoadResult;
	};

}  // namespace howdy::native
