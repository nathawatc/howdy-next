#pragma once

#include "support/eintr.hpp"
#include "support/scoped_fd.hpp"

#include <filesystem>
#include <utility>

#include <sys/file.h>

namespace howdy::native {

	class ScopedFileLock {
	public:
		ScopedFileLock() = default;

		explicit ScopedFileLock(ScopedFd fd)
		    : fd_(std::move(fd)) {}

		ScopedFileLock(const ScopedFileLock &)                     = delete;
		auto operator=(const ScopedFileLock &) -> ScopedFileLock & = delete;

		ScopedFileLock(ScopedFileLock &&other) noexcept
		    : fd_(std::move(other.fd_)) {}

		auto operator=(ScopedFileLock &&other) noexcept -> ScopedFileLock & {
			if (this != &other) {
				Release();
				fd_ = std::move(other.fd_);
			}
			return *this;
		}

		~ScopedFileLock() {
			Release();
		}

		[[nodiscard]] auto Get() const noexcept -> int {
			return fd_.Get();
		}

		void Release() {
			if (!fd_.Valid()) {
				return;
			}
			(void)RetryOnEintr([this] -> int {
				return flock(fd_.Get(), LOCK_UN);
			});
			fd_.Reset();
		}

	private:
		ScopedFd fd_;
	};

	inline auto LockFilePath(const std::filesystem::path &target_path) -> std::filesystem::path {
		return target_path.string() + ".lock";
	}

}  // namespace howdy::native
