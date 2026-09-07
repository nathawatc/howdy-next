#pragma once

#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <unistd.h>
#include <utility>

#include <sys/file.h>

namespace howdy::native {

	struct ScopedFileLock {
		int                   fd = -1;
		std::filesystem::path path;

		ScopedFileLock()                                           = default;
		ScopedFileLock(const ScopedFileLock &)                     = delete;
		auto operator=(const ScopedFileLock &) -> ScopedFileLock & = delete;

		ScopedFileLock(ScopedFileLock &&other) noexcept
		    : fd(other.fd)
		    , path(std::move(other.path)) {
			other.fd = -1;
		}

		auto operator=(ScopedFileLock &&other) noexcept -> ScopedFileLock & {
			if (this != &other) {
				Release();
				fd       = other.fd;
				path     = std::move(other.path);
				other.fd = -1;
			}
			return *this;
		}

		~ScopedFileLock() {
			Release();
		}

		void Release() {
			if (fd < 0) {
				return;
			}
			while (flock(fd, LOCK_UN) != 0 && errno == EINTR) {
			}
			close(fd);
			fd = -1;
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
		const int  fd =
		    open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
		if (fd < 0) {
			return std::nullopt;
		}

		while (flock(fd, LOCK_EX) != 0) {
			if (errno != EINTR) {
				close(fd);
				return std::nullopt;
			}
		}

		ScopedFileLock lock;
		lock.fd   = fd;
		lock.path = path;
		return lock;
	}

}  // namespace howdy::native
