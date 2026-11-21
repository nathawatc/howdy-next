#pragma once

namespace howdy::native::cli_internal {

	// Asks "Continue? [y/N]" on stdin/stdout; only "y" or "Y" confirms.
	[[nodiscard]] auto ConfirmInteractiveAction() -> bool;

}  // namespace howdy::native::cli_internal
