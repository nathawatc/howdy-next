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

		[[nodiscard]] auto write_document(const user_model_codec::Document &document) const
		    -> AtomicFileCommitResult;
		[[nodiscard]] auto remove_file() const -> AtomicFileCommitResult;

	private:
		std::filesystem::path path_;
		ScopedFileLock        namespace_lock_;
		ScopedFileLock        lock_;
		bool                  created_empty_file_ = false;
		mutable bool          completed_          = false;

		UserModelStoreTransaction(std::filesystem::path path, ScopedFileLock namespace_lock,
		                          ScopedFileLock lock, bool created_empty_file);

		[[nodiscard]] auto path() const -> const std::filesystem::path &;
		[[nodiscard]] auto path_matches_locked_file() const -> bool;
		[[nodiscard]] auto snapshot() const -> std::optional<UserModelFileSnapshot>;
		[[nodiscard]] auto snapshot_matches(const UserModelFileSnapshot &expected) const
		    -> std::optional<bool>;

		friend class UserModelStore;
		friend auto
		clear_user_model_entries_if_unchanged(const std::string           &user,
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

		[[nodiscard]] static auto begin_mutation(const std::string &user)
		    -> UserModelStoreMutationResult;
		[[nodiscard]] static auto lock_existing(const std::string &user)
		    -> UserModelStoreTransactionResult;

	private:
		struct UserModelExpectations {
			std::string backend;
			std::string metric;
			std::string model;
			bool        strict_shape;
		};

		struct UserModelPathResult {
			UserModelStatus       status = UserModelStatus::kOk;
			std::string           error_message;
			std::filesystem::path path;
		};

		[[nodiscard]] static auto resolve(const std::string &user, bool create_directory,
		                                  std::optional<uid_t> owner_uid) -> UserModelPathResult;
		[[nodiscard]] static auto load_document(const std::string           &user,
		                                        const UserModelExpectations &expectations,
		                                        std::optional<uid_t>         owner_uid)
		    -> user_model_codec::Document;
		[[nodiscard]] static auto inspect(const std::string &user) -> UserModelInspectResult;
		[[nodiscard]] static auto load_document_from_path(const std::filesystem::path &path,
		                                                  const UserModelExpectations &expectations)
		    -> user_model_codec::Document;
		[[nodiscard]] static auto load_document_from_fd(int fd, const std::filesystem::path &path,
		                                                const UserModelExpectations &expectations,
		                                                bool treat_empty_as_no_model)
		    -> user_model_codec::Document;
		[[nodiscard]] static auto inspect_regular_file_status(const std::filesystem::path &path,
		                                                      std::string                 *message)
		    -> UserModelStatus;

		friend auto list_user_model_entries(const std::string &user,
		                                    const std::string &expected_backend,
		                                    const std::string &expected_metric,
		                                    const std::string &expected_model)
		    -> UserModelListResult;
		friend auto inspect_user_model_file(const std::string &user) -> UserModelInspectResult;
		friend auto load_user_models(const std::string &user, const std::string &expected_backend,
		                             std::optional<uid_t> owner_uid) -> UserModelLoadResult;
	};

}  // namespace howdy::native
