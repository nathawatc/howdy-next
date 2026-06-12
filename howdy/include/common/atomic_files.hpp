#pragma once

#include "common/fd_io.hpp"

#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

#include <sys/stat.h>

namespace howdy::native {

	inline constexpr mode_t kDefaultAtomicFileMode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;

	class ScopedFd {
	public:
		ScopedFd() = default;

		explicit ScopedFd(int fd)
		    : fd_(fd) {}

		ScopedFd(const ScopedFd &)                     = delete;
		auto operator=(const ScopedFd &) -> ScopedFd & = delete;

		ScopedFd(ScopedFd &&other) noexcept
		    : fd_(other.release()) {}

		auto operator=(ScopedFd &&other) noexcept -> ScopedFd & {
			if (this != &other) {
				reset(other.release());
			}
			return *this;
		}

		~ScopedFd() {
			reset();
		}

		[[nodiscard]] auto get() const -> int {
			return fd_;
		}

		[[nodiscard]] auto close() -> bool {
			if (fd_ < 0) {
				return true;
			}
			const int fd = release();
			return ::close(fd) == 0;
		}

		void reset(int fd = -1) {
			if (fd_ >= 0) {
				::close(fd_);
			}
			fd_ = fd;
		}

		[[nodiscard]] auto release() -> int {
			const int fd = fd_;
			fd_          = -1;
			return fd;
		}

	private:
		int fd_ = -1;
	};

	struct StagedFile {
		ScopedFd              fd;
		std::filesystem::path path;
	};

	inline void sync_parent_directory(const std::filesystem::path &path) {
		const int dir_fd = open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
		if (dir_fd >= 0) {
			fsync(dir_fd);
			close(dir_fd);
		}
	}

	inline auto remove_file_and_sync(const std::filesystem::path &path) -> bool {
		std::error_code ec;
		std::filesystem::remove(path, ec);
		if (ec) {
			return false;
		}
		sync_parent_directory(path);
		return true;
	}

	inline void cleanup_staged_file(StagedFile &staged) {
		staged.fd.reset();
		std::error_code ec;
		std::filesystem::remove(staged.path, ec);
	}

	inline auto prepare_staged_file(const std::filesystem::path &destination,
	                                std::string_view             temp_prefix,
	                                mode_t default_mode = kDefaultAtomicFileMode)
	    -> std::optional<StagedFile> {
		const auto      parent = destination.parent_path();
		std::error_code create_ec;
		std::filesystem::create_directories(parent, create_ec);
		if (create_ec) {
			return std::nullopt;
		}

		struct stat current_stat{};
		const bool  have_current_stat = lstat(destination.c_str(), &current_stat) == 0;
		if (have_current_stat && !S_ISREG(current_stat.st_mode)) {
			return std::nullopt;
		}

		std::string       temp_template = (parent / (std::string(temp_prefix) + "XXXXXX")).string();
		std::vector<char> writable(temp_template.begin(), temp_template.end());
		writable.push_back('\0');

		ScopedFd fd(mkstemp(writable.data()));
		if (fd.get() < 0) {
			return std::nullopt;
		}

		const std::filesystem::path temp_path(writable.data());
		if (have_current_stat) {
			if (fchmod(fd.get(), current_stat.st_mode & 07777) != 0 ||
			    fchown(fd.get(), current_stat.st_uid, current_stat.st_gid) != 0) {
				fd.reset();
				std::error_code ec;
				std::filesystem::remove(temp_path, ec);
				return std::nullopt;
			}
		} else if (fchmod(fd.get(), default_mode) != 0) {
			fd.reset();
			std::error_code ec;
			std::filesystem::remove(temp_path, ec);
			return std::nullopt;
		}

		return StagedFile{.fd = std::move(fd), .path = temp_path};
	}

	inline auto install_staged_file(StagedFile &staged, const std::filesystem::path &destination)
	    -> bool {
		if (staged.path.empty()) {
			staged.fd.reset();
			return false;
		}

		if (staged.fd.get() >= 0 && fsync(staged.fd.get()) != 0) {
			cleanup_staged_file(staged);
			return false;
		}

		if (!staged.fd.close()) {
			cleanup_staged_file(staged);
			return false;
		}

		std::error_code ec;
		std::filesystem::rename(staged.path, destination, ec);
		if (ec) {
			cleanup_staged_file(staged);
			return false;
		}
		sync_parent_directory(destination);
		staged.path.clear();
		return true;
	}

	inline auto write_atomic_file(const std::filesystem::path &path, std::string_view content,
	                              mode_t default_mode = kDefaultAtomicFileMode) -> bool {
		const auto parent = path.parent_path();
		std::filesystem::create_directories(parent);

		struct stat current_stat{};
		const bool  have_current_stat = lstat(path.c_str(), &current_stat) == 0;
		if (have_current_stat && !S_ISREG(current_stat.st_mode)) {
			return false;
		}

		std::string       temp_template = (parent / ".howdy-atomic-XXXXXX").string();
		std::vector<char> writable(temp_template.begin(), temp_template.end());
		writable.push_back('\0');

		const int fd = mkstemp(writable.data());
		if (fd < 0) {
			return false;
		}

		const std::filesystem::path temp_path(writable.data());
		bool                        ok = true;
		if (have_current_stat) {
			if (fchmod(fd, current_stat.st_mode & 07777) != 0 ||
			    fchown(fd, current_stat.st_uid, current_stat.st_gid) != 0) {
				ok = false;
			}
		} else if (fchmod(fd, default_mode) != 0) {
			ok = false;
		}

		if (ok && !write_all_to_fd(fd, content)) {
			ok = false;
		}

		if (ok && fsync(fd) != 0) {
			ok = false;
		}
		close(fd);

		if (!ok) {
			std::error_code ec;
			std::filesystem::remove(temp_path, ec);
			return false;
		}

		std::error_code ec;
		std::filesystem::rename(temp_path, path, ec);
		if (ec) {
			std::filesystem::remove(temp_path, ec);
			return false;
		}

		sync_parent_directory(path);
		return true;
	}

}  // namespace howdy::native
