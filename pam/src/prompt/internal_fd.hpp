#pragma once

namespace howdy::pam::detail {

	class ScopedFd {
	public:
		ScopedFd() noexcept = default;
		explicit ScopedFd(int fd) noexcept;
		~ScopedFd();

		ScopedFd(const ScopedFd &)                     = delete;
		auto operator=(const ScopedFd &) -> ScopedFd & = delete;
		ScopedFd(ScopedFd &&other) noexcept;
		auto operator=(ScopedFd &&other) noexcept -> ScopedFd &;

		[[nodiscard]] auto get() const noexcept -> int;
		[[nodiscard]] auto valid() const noexcept -> bool;
		auto               release() noexcept -> int;

	private:
		int fd_ = -1;
	};

	struct InternalFdOperations {
		void *context                                               = nullptr;
		int (*duplicate)(void *context, int fd, int minimum_fd)     = nullptr;
		int (*create_pipe)(void *context, int *pipe_fds, int flags) = nullptr;
	};

	struct InternalPipe {
		ScopedFd read;
		ScopedFd write;

		[[nodiscard]] auto valid() const noexcept -> bool;
	};

	[[nodiscard]] auto
	normalize_internal_fd(ScopedFd fd, const InternalFdOperations *operations = nullptr) noexcept
	    -> ScopedFd;
	[[nodiscard]] auto
	create_internal_pipe(int flags, const InternalFdOperations *operations = nullptr) noexcept
	    -> InternalPipe;

}  // namespace howdy::pam::detail
