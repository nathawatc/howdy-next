#pragma once

#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#include <sys/stat.h>

namespace howdy::native {

	inline constexpr mode_t kDefaultAtomicFileMode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;

	inline auto write_all_to_fd(int fd, std::string_view content) -> bool {
		const char *cursor    = content.data();
		std::size_t remaining = content.size();
		while (remaining > 0) {
			const auto bytes_written = write(fd, cursor, remaining);
			if (bytes_written < 0) {
				if (errno == EINTR) {
					continue;
				}
				return false;
			}
			cursor += bytes_written;
			remaining -= static_cast<std::size_t>(bytes_written);
		}
		return true;
	}

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
