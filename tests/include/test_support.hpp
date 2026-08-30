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

	inline auto expect(bool condition, std::string_view message) -> bool {
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

	template <typename Actual, typename Expected, typename Tolerance>
	inline auto expect_near(Actual actual, Expected expected, Tolerance tolerance,
	                        std::string_view message) -> bool {
		return expect(std::fabs(actual - expected) <= tolerance, message);
	}

	inline auto read_file(const std::filesystem::path &path) -> std::string {
		std::ifstream input(path, std::ios::binary);
		if (!input.is_open()) {
			return {};
		}
		return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	}

	inline auto write_file(const std::filesystem::path &path, std::string_view content) -> bool {
		std::ofstream output(path, std::ios::binary);
		if (!output.is_open()) {
			return false;
		}
		output.write(content.data(), static_cast<std::streamsize>(content.size()));
		return output.good();
	}

	inline auto count_files_with_prefix(const std::filesystem::path &directory,
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
