#pragma once

#include "../user_model_codec/internal.hpp"
#include "storage/user_models.hpp"
#include "support/atomic_files.hpp"
#include "support/file_lock.hpp"
#include "support/scoped_fd.hpp"

#include <filesystem>
#include <optional>
#include <string>

#include <sys/types.h>

namespace howdy::native {

	// "Failed to inspect user model file: <path> (<errno text>)".
	[[nodiscard]] auto UserModelInspectionFailedMessage(const std::filesystem::path &path,
	                                                    int error_number) -> std::string;

	class UserModelStoreTransaction {
	public:
		UserModelStoreTransaction(const UserModelStoreTransaction &)                     = delete;
		auto operator=(const UserModelStoreTransaction &) -> UserModelStoreTransaction & = delete;

		UserModelStoreTransaction(UserModelStoreTransaction &&) noexcept = default;
		auto operator=(UserModelStoreTransaction &&) noexcept
		    -> UserModelStoreTransaction & = default;
		~UserModelStoreTransaction()       = default;

		[[nodiscard]] auto WriteDocument(const user_model_codec::Document &document) const
		    -> AtomicFileCommitResult;
		[[nodiscard]] auto RemoveFile() const -> AtomicFileCommitResult;

	private:
		std::filesystem::path path_;
		ScopedFileLock        namespace_lock_;
		// The model file, held open to check that the path still names it, or not valid when no
		// model existed and a first one is to be linked in. Not locked: the root-only
		// namespace lock serializes writers, and a user holding a descriptor to the file from
		// the auth helper could otherwise stall them with flock.
		ScopedFd file_;

		UserModelStoreTransaction(std::filesystem::path path, ScopedFileLock namespace_lock,
		                          ScopedFd file);

		[[nodiscard]] auto Path() const -> const std::filesystem::path &;
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
		user_model_codec::Document               document;
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
		struct UserModelExpectations {
			std::string backend;
			std::string model;
			bool        strict_shape;
		};

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
		LoadDocument(const std::string &user, const UserModelExpectations &expectations,
		             std::optional<uid_t>                          owner_uid,
		             const file_security_internal::ValidationRoot &validation_root)
		    -> user_model_codec::Document;
		[[nodiscard]] static auto
		Inspect(const std::string                            &user,
		        const file_security_internal::ValidationRoot &validation_root)
		    -> UserModelInspectResult;
		[[nodiscard]] static auto LoadDocumentFromPath(const std::filesystem::path &path,
		                                               const UserModelExpectations &expectations,
		                                               std::optional<uid_t>         owner_uid)
		    -> user_model_codec::Document;
		// Validates a descriptor that is already open, then reads its document. A nullopt
		// `owner_uid` skips the owner check.
		[[nodiscard]] static auto LoadDocumentFromFd(int fd, const std::filesystem::path &path,
		                                             const UserModelExpectations &expectations,
		                                             std::optional<uid_t>         owner_uid)
		    -> user_model_codec::Document;
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
		friend auto LoadUserModelsFromFd(int model_fd, const std::filesystem::path &display_path,
		                                 const std::string &expected_backend, uid_t owner_uid)
		    -> UserModelLoadResult;
	};

}  // namespace howdy::native
