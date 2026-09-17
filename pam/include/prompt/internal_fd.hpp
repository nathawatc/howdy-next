#pragma once

#include "support/scoped_fd.hpp"

namespace howdy::pam::detail {

	using ScopedFd = howdy::native::ScopedFd;

	struct InternalFdOperations {
		void *context                                               = nullptr;
		int (*duplicate)(void *context, int fd, int minimum_fd)     = nullptr;
		int (*create_pipe)(void *context, int *pipe_fds, int flags) = nullptr;
	};

	struct InternalPipe {
		ScopedFd read;
		ScopedFd write;

		[[nodiscard]] auto Valid() const noexcept -> bool;
	};

	[[nodiscard]] auto
	NormalizeInternalFd(ScopedFd fd, const InternalFdOperations *operations = nullptr) noexcept
	    -> ScopedFd;
	[[nodiscard]] auto CreateInternalPipe(int                         flags,
	                                      const InternalFdOperations *operations = nullptr) noexcept
	    -> InternalPipe;

}  // namespace howdy::pam::detail
