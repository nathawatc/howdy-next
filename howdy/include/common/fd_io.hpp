#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <string>
#include <string_view>
#include <unistd.h>

namespace howdy::native {

	struct BoundedReadResult {
		std::string output;
		bool        hit_limit    = false;
		bool        read_error   = false;
		int         error_number = 0;
	};

	inline auto write_all_to_fd(int fd, const char *data, std::size_t size) -> bool {
		if (data == nullptr && size > 0) {
			return false;
		}
		const char *cursor    = data;
		std::size_t remaining = size;
		while (remaining > 0) {
			const auto bytes_written = write(fd, cursor, remaining);
			if (bytes_written < 0) {
				if (errno == EINTR) {
					continue;
				}
				return false;
			}
			if (bytes_written == 0) {
				return false;
			}
			cursor += bytes_written;
			remaining -= static_cast<std::size_t>(bytes_written);
		}
		return true;
	}

	inline auto write_all_to_fd(int fd, std::string_view content) -> bool {
		return write_all_to_fd(fd, content.data(), content.size());
	}

	inline auto read_fd_to_string_bounded(int fd, std::size_t max_bytes) -> BoundedReadResult {
		BoundedReadResult result;
		if (max_bytes == 0) {
			return result;
		}

		std::array<char, 1024> buffer{};
		while (result.output.size() < max_bytes) {
			const std::size_t remaining     = max_bytes - result.output.size();
			const std::size_t bytes_to_read = std::min(remaining, buffer.size());
			const ssize_t     bytes_read    = read(fd, buffer.data(), bytes_to_read);
			if (bytes_read < 0) {
				if (errno == EINTR) {
					continue;
				}
				result.read_error   = true;
				result.error_number = errno;
				break;
			}
			if (bytes_read == 0) {
				break;
			}

			result.output.append(buffer.data(), static_cast<std::size_t>(bytes_read));
		}

		result.hit_limit = result.output.size() == max_bytes;
		return result;
	}

}  // namespace howdy::native
