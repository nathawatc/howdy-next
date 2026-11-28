#pragma once

#include <filesystem>
#include <functional>
#include <optional>

namespace howdy::native::user_model_store_test_hooks {

	struct Hooks {
		// Runs after the read-only path walk succeeds and before LoadDocument/InspectFile open().
		std::function<void(const std::filesystem::path &)> before_read_open;
		std::function<void(const std::filesystem::path &)> before_lock;
		std::function<void(const std::filesystem::path &)> after_lock_before_revalidate;
		// Runs after the post-lock recheck, before a write stages its replacement.
		std::function<void(const std::filesystem::path &)> before_write_staging;
		std::function<void(const std::filesystem::path &)> before_write_commit;
		// Runs after the identity check on an update, and at the same point (with no identity
		// to check) before a first model is linked in.
		std::function<void(const std::filesystem::path &)> after_write_identity_check;
		std::function<void(const std::filesystem::path &)> before_delete_commit;
		bool                                               fail_write         = false;
		bool                                               fail_fsync         = false;
		bool                                               fail_parent_sync   = false;
		bool                                               fail_write_cleanup = false;
		// Fails the unlink of a model a removal moved aside; the removal moves it back.
		bool               fail_delete_unlink = false;
		std::optional<int> exchange_errno;
		std::optional<int> rollback_exchange_errno;
		// Stands in for the RENAME_NOREPLACE that links a first model in.
		std::optional<int> create_errno;
	};

}  // namespace howdy::native::user_model_store_test_hooks
