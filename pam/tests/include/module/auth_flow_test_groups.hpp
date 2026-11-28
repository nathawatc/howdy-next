#pragma once

namespace howdy::test::auth_flow {

	// Scoped override for pam_get_item(..., PAM_AUTHTOK, ...) via --wrap=pam_get_item so tests can
	// exercise module-context PAM_AUTHTOK lookups without poking into libpam internals.
	class ScopedAuthTokenItemOverride {
	public:
		ScopedAuthTokenItemOverride(int result, const void *token) noexcept;
		~ScopedAuthTokenItemOverride();

		ScopedAuthTokenItemOverride(const ScopedAuthTokenItemOverride &) = delete;
		auto operator=(const ScopedAuthTokenItemOverride &)
		    -> ScopedAuthTokenItemOverride &                                            = delete;
		ScopedAuthTokenItemOverride(ScopedAuthTokenItemOverride &&)                     = delete;
		auto operator=(ScopedAuthTokenItemOverride &&) -> ScopedAuthTokenItemOverride & = delete;

	private:
		bool        previous_enabled_ = false;
		int         previous_result_  = 0;
		const void *previous_token_   = nullptr;
	};

}  // namespace howdy::test::auth_flow

auto RunAuthFlowIntegrationTests() -> bool;
