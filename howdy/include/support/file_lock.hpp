#pragma once

#include "support/scoped_fd.hpp"

#include <cerrno>
#include <filesystem>
#include <utility>

#include <sys/file.h>

namespace howdy::native {

	struct ScopedFileLock {
		ScopedFd              fd;
		std::filesystem::path path;

		ScopedFileLock()                                           = default;
		ScopedFileLock(const ScopedFileLock &)                     = delete;
		auto operator=(const ScopedFileLock &) -> ScopedFileLock & = delete;

		ScopedFileLock(ScopedFileLock &&other) noexcept
		    : fd(std::move(other.fd))
		    , path(std::move(other.path)) {}

		auto operator=(ScopedFileLock &&other) noexcept -> ScopedFileLock & {
			if (this != &other) {
				Release();
				fd   = std::move(other.fd);
				path = std::move(other.path);
			}
			return *this;
		}

		~ScopedFileLock() {
			Release();
		}

		void Release() {
			if (!fd.Valid()) {
				return;
			}
			while (flock(fd.Get(), LOCK_UN) != 0 && errno == EINTR) {
			}
			fd.Reset();
		}
	};

	inline auto LockFilePath(const std::filesystem::path &target_path) -> std::filesystem::path {
		return target_path.string() + ".lock";
	}

}  // namespace howdy::native
