#pragma once

#include <filesystem>
#include <functional>

namespace howdy::native::user_model_store_test_hooks {

	struct Hooks {
		std::function<void(const std::filesystem::path &)> before_lock;
		std::function<void(const std::filesystem::path &)> after_lock_before_revalidate;
		std::function<void(const std::filesystem::path &)> before_write_commit;
		std::function<void(const std::filesystem::path &)> after_write_identity_check;
		std::function<void(const std::filesystem::path &)> before_delete_commit;
		bool                                               fail_write         = false;
		bool                                               fail_fsync         = false;
		bool                                               fail_write_cleanup = false;
		bool                                               fail_delete_unlink = false;
	};

	[[nodiscard]] auto current() -> Hooks &;

	class ScopedHooks {
	public:
		explicit ScopedHooks(Hooks hooks);
		ScopedHooks(const ScopedHooks &)                     = delete;
		auto operator=(const ScopedHooks &) -> ScopedHooks & = delete;
		ScopedHooks(ScopedHooks &&)                          = delete;
		auto operator=(ScopedHooks &&) -> ScopedHooks &      = delete;
		~ScopedHooks();

	private:
		Hooks previous_;
	};

}  // namespace howdy::native::user_model_store_test_hooks
