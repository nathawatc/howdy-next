#pragma once

#include <functional>

namespace howdy::native::config_test_hooks {

	using AfterOpenBeforeRead = std::function<void()>;

	[[nodiscard]] auto current() -> AfterOpenBeforeRead &;

	class ScopedHooks {
	public:
		explicit ScopedHooks(AfterOpenBeforeRead hook);
		ScopedHooks(const ScopedHooks &)                     = delete;
		auto operator=(const ScopedHooks &) -> ScopedHooks & = delete;
		ScopedHooks(ScopedHooks &&)                          = delete;
		auto operator=(ScopedHooks &&) -> ScopedHooks &      = delete;
		~ScopedHooks();

	private:
		AfterOpenBeforeRead previous_;
	};

}  // namespace howdy::native::config_test_hooks
