#include "common/fd_io.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

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
				close(fd_);
			}
			fd_ = fd;
		}

		auto release() -> int {
			const int fd = fd_;
			fd_          = -1;
			return fd;
		}

	private:
		int fd_ = -1;
	};

	struct TemporaryFile {
		std::filesystem::path path;
		ScopedFd              fd;
	};

	auto expect(bool condition, const std::string &message) -> bool {
		if (!condition) {
			std::cerr << "FAIL: " << message << "\n";
			return false;
		}
		return true;
	}

	auto read_file(const std::filesystem::path &path) -> std::string {
		std::ifstream input(path);
		return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	}

	auto create_temp_file(const std::filesystem::path &root, const std::string &label)
	    -> std::optional<TemporaryFile> {
		std::string       templ = (root / ("fd-io-" + label + "-XXXXXX")).string();
		std::vector<char> buffer(templ.begin(), templ.end());
		buffer.push_back('\0');

		ScopedFd fd(mkstemp(buffer.data()));
		if (fd.get() < 0) {
			return std::nullopt;
		}

		return TemporaryFile{.path = buffer.data(), .fd = std::move(fd)};
	}

	auto expect_empty_write(const std::filesystem::path &temp_root) -> bool {
		bool ok   = true;
		auto file = create_temp_file(temp_root, "empty");
		ok &= expect(file.has_value(), "creates empty-write file");
		if (!file.has_value()) {
			return false;
		}

		const std::string empty;
		ok &= expect(howdy::native::write_all_to_fd(file->fd.get(), empty.data(), empty.size()),
		             "empty write succeeds");
		file->fd.reset();
		ok &= expect(read_file(file->path).empty(), "empty write leaves file empty");

		return ok;
	}

	auto expect_invalid_fd_failure() -> bool {
		const std::string content = "x";
		return expect(!howdy::native::write_all_to_fd(-1, content.data(), content.size()),
		              "invalid fd fails");
	}

	auto open_pipe(std::array<ScopedFd, 2> *fds) -> bool {
		std::array<int, 2> raw_fds{{-1, -1}};
		if (pipe(raw_fds.data()) != 0) {
			return false;
		}
		(*fds)[0].reset(raw_fds[0]);
		(*fds)[1].reset(raw_fds[1]);
		return true;
	}

	auto expect_bounded_reading(const std::filesystem::path &temp_root) -> bool {
		bool                    ok = true;
		std::array<ScopedFd, 2> empty_pipe;
		ok &= expect(open_pipe(&empty_pipe), "creates empty read pipe");
		empty_pipe[1].reset();
		ok &= expect(howdy::native::read_fd_to_string_bounded(empty_pipe[0].get(), 1024).empty(),
		             "reads empty input");
		ok &= expect(howdy::native::read_fd_to_string_bounded(-1, 1024).empty(),
		             "invalid fd returns collected empty output");

		std::array<ScopedFd, 2> small_pipe;
		ok &= expect(open_pipe(&small_pipe), "creates small read pipe");
		const std::string small_output = "small helper output\n";
		ok &= expect(howdy::native::write_all_to_fd(small_pipe[1].get(), small_output),
		             "writes small helper output");
		small_pipe[1].reset();
		ok &= expect(howdy::native::read_fd_to_string_bounded(small_pipe[0].get(), 1024) ==
		                 small_output,
		             "reads complete small helper output");

		auto bounded_file = create_temp_file(temp_root, "bounded");
		ok &= expect(bounded_file.has_value(), "creates bounded input file");
		if (!bounded_file.has_value()) {
			return false;
		}
		unlink(bounded_file->path.c_str());
		const std::string oversized_output(16384, 'x');
		ok &= expect(howdy::native::write_all_to_fd(bounded_file->fd.get(), oversized_output),
		             "writes oversized helper output");
		ok &= expect(lseek(bounded_file->fd.get(), 0, SEEK_SET) == 0,
		             "rewinds oversized helper output");
		const std::string bounded_output =
		    howdy::native::read_fd_to_string_bounded(bounded_file->fd.get(), 9216);
		ok &= expect(bounded_output == oversized_output.substr(0, 9216),
		             "stops reading after bounded output threshold");

		return ok;
	}

	auto expect_full_write(const std::filesystem::path &temp_root) -> bool {
		bool ok   = true;
		auto file = create_temp_file(temp_root, "full");
		ok &= expect(file.has_value(), "creates full-write file");
		if (!file.has_value()) {
			return false;
		}

		const std::string content = "alpha\nbeta\ngamma\n";
		ok &=
		    expect(howdy::native::write_all_to_fd(file->fd.get(), content), "full write succeeds");
		file->fd.reset();
		ok &= expect(read_file(file->path) == content, "full write preserves content");

		return ok;
	}

}  // namespace

auto main() -> int {
	namespace fs = std::filesystem;

	bool            ok        = true;
	const auto      temp_root = fs::current_path() / "howdy-fd-io-test";
	std::error_code ec;
	fs::remove_all(temp_root, ec);
	fs::create_directories(temp_root, ec);
	ok &= expect(!ec, "create temp root");

	ok &= expect_empty_write(temp_root);
	ok &= expect_invalid_fd_failure();
	ok &= expect_full_write(temp_root);
	ok &= expect_bounded_reading(temp_root);

	fs::remove_all(temp_root, ec);
	return ok ? 0 : 1;
}
