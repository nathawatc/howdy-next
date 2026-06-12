#pragma once

#include <cerrno>
#include <cstddef>
#include <string_view>
#include <unistd.h>

namespace howdy::native {

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

}  // namespace howdy::native
