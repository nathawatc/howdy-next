#pragma once

#include "support/scoped_fd.hpp"

#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <unistd.h>
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

	inline auto AcquireFileLock(const std::filesystem::path &target_path)
	    -> std::optional<ScopedFileLock> {
		std::error_code ec;
		std::filesystem::create_directories(target_path.parent_path(), ec);
		if (ec) {
			return std::nullopt;
		}

		const auto path = LockFilePath(target_path);
		ScopedFd   fd(
		    open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR));
		if (!fd.Valid()) {
			return std::nullopt;
		}

		while (flock(fd.Get(), LOCK_EX) != 0) {
			if (errno != EINTR) {
				return std::nullopt;
			}
		}

		ScopedFileLock lock;
		lock.fd   = std::move(fd);
		lock.path = path;
		return lock;
	}

}  // namespace howdy::native
