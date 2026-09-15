#pragma once

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <unistd.h>

namespace howdy::test {

	inline auto Expect(bool condition, std::string_view message) -> bool {
		if (!condition) {
			std::cerr << "FAIL: " << message << '\n';
		}
		return condition;
	}

	class ScopedFd {
	public:
		ScopedFd() = default;

		explicit ScopedFd(int fd)
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

		[[nodiscard]] auto Get() const -> int {
			return fd_;
		}

		void Reset(int fd = -1) {
			if (fd_ >= 0) {
				::close(fd_);
			}
			fd_ = fd;
		}

		[[nodiscard]] auto Release() -> int {
			const int fd = fd_;
			fd_          = -1;
			return fd;
		}

	private:
		int fd_ = -1;
	};

	template <typename Actual, typename Expected, typename Tolerance>
	inline auto ExpectNear(Actual actual, Expected expected, Tolerance tolerance,
	                       std::string_view message) -> bool {
		return Expect(std::fabs(actual - expected) <= tolerance, message);
	}

	inline auto ReadFile(const std::filesystem::path &path) -> std::string {
		std::ifstream input(path, std::ios::binary);
		if (!input.is_open()) {
			return {};
		}
		return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	}

	inline auto WriteFile(const std::filesystem::path &path, std::string_view content) -> bool {
		std::ofstream output(path, std::ios::binary);
		if (!output.is_open()) {
			return false;
		}
		output.write(content.data(), static_cast<std::streamsize>(content.size()));
		return output.good();
	}

	inline auto CountFilesWithPrefix(const std::filesystem::path &directory,
	                                 std::string_view             prefix) -> std::size_t {
		std::size_t count = 0;
		for (const auto &entry : std::filesystem::directory_iterator(directory)) {
			if (entry.path().filename().string().starts_with(prefix)) {
				++count;
			}
		}
		return count;
	}

}  // namespace howdy::test
