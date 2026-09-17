#pragma once

#include <unistd.h>

namespace howdy::native {

	class ScopedFd {
	public:
		ScopedFd() noexcept = default;

		explicit ScopedFd(int fd) noexcept
		    : fd_(fd) {}

		ScopedFd(const ScopedFd &)                     = delete;
		auto operator=(const ScopedFd &) -> ScopedFd & = delete;

		ScopedFd(ScopedFd &&other) noexcept
		    : fd_(other.Release()) {}

		auto operator=(ScopedFd &&other) noexcept -> ScopedFd & {
			if (this != &other) {
				Reset(other.Release());
			}
			return *this;
		}

		~ScopedFd() {
			Reset();
		}

		[[nodiscard]] auto Get() const noexcept -> int {
			return fd_;
		}

		[[nodiscard]] auto Valid() const noexcept -> bool {
			return fd_ >= 0;
		}

		[[nodiscard]] auto Close() noexcept -> bool {
			if (fd_ < 0) {
				return true;
			}
			const int fd = Release();
			return ::close(fd) == 0;
		}

		auto Reset(int fd = -1) noexcept -> void {
			if (fd_ >= 0) {
				(void)::close(fd_);
			}
			fd_ = fd;
		}

		[[nodiscard]] auto Release() noexcept -> int {
			const int fd = fd_;
			fd_          = -1;
			return fd;
		}

	private:
		int fd_ = -1;
	};

}  // namespace howdy::native
