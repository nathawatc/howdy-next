#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <unistd.h>

#include <sys/stat.h>

namespace howdy::native {

	struct BoundedReadResult {
		std::string output;
		bool        hit_limit    = false;
		bool        read_error   = false;
		int         error_number = 0;
	};

	struct BoundedReadRequest {
		int         fd        = -1;
		std::size_t max_bytes = 0;
	};

	inline auto WriteAllToFd(int fd, std::span<const std::byte> data) -> bool {
		auto remaining = data;
		while (!remaining.empty()) {
			const auto bytes_written = write(fd, remaining.data(), remaining.size());
			if (bytes_written < 0) {
				if (errno == EINTR) {
					continue;
				}
				return false;
			}
			if (bytes_written == 0) {
				return false;
			}
			remaining = remaining.subspan(static_cast<std::size_t>(bytes_written));
		}
		return true;
	}

	inline auto WriteAllToFd(int fd, const char *data, std::size_t size) -> bool {
		if (size == 0) {
			return true;
		}
		if (data == nullptr) {
			return false;
		}
		return WriteAllToFd(fd, std::as_bytes(std::span<const char>(data, size)));
	}

	inline auto WriteAllToFd(int fd, std::string_view content) -> bool {
		return WriteAllToFd(fd, content.data(), content.size());
	}

	inline auto SyncFd(int fd) -> bool {
		if (fd < 0) {
			return false;
		}

		while (fsync(fd) != 0) {
			if (errno != EINTR) {
				return false;
			}
		}
		return true;
	}

	inline auto ReadFdToStringBounded(BoundedReadRequest request) -> BoundedReadResult {
		BoundedReadResult result;
		if (request.max_bytes == 0) {
			return result;
		}

		// Size is only an allocation hint: read until EOF or the bound, even if the file grows.
		struct stat info{};
		if (request.fd >= 0 && fstat(request.fd, &info) == 0 && S_ISREG(info.st_mode) &&
		    info.st_size > 0) {
			result.output.reserve(static_cast<std::size_t>(std::min<std::uintmax_t>(
			    static_cast<std::uintmax_t>(info.st_size), request.max_bytes)));
		}

		std::array<char, 16384> buffer{};
		while (result.output.size() < request.max_bytes) {
			const std::size_t remaining     = request.max_bytes - result.output.size();
			const std::size_t bytes_to_read = std::min(remaining, buffer.size());
			const ssize_t     bytes_read    = read(request.fd, buffer.data(), bytes_to_read);
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

		result.hit_limit = result.output.size() == request.max_bytes;
		return result;
	}

}  // namespace howdy::native
