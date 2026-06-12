#include "common/fd_io.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

#include <sys/wait.h>

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

	auto wait_for_child(pid_t child_pid) -> bool {
		while (true) {
			int         status = 0;
			const pid_t result = waitpid(child_pid, &status, 0);
			if (result == child_pid) {
				return WIFEXITED(status) || WIFSIGNALED(status);
			}
			if (result < 0 && errno == EINTR) {
				continue;
			}
			return false;
		}
	}

	auto expect_bounded_reading(const std::filesystem::path &temp_root) -> bool {
		bool                    ok     = true;
		constexpr std::size_t   kBound = 1500;
		const std::string       exact_output(kBound, 'a');
		const std::string       oversized_output(4096, 'x');
		std::array<ScopedFd, 2> empty_pipe;
		ok &= expect(open_pipe(&empty_pipe), "creates empty read pipe");
		empty_pipe[1].reset();
		const auto empty_result =
		    howdy::native::read_fd_to_string_bounded(empty_pipe[0].get(), kBound);
		ok &= expect(empty_result.output.empty(), "reads empty input");
		ok &= expect(!empty_result.hit_limit, "empty input does not hit limit");
		const auto invalid_result = howdy::native::read_fd_to_string_bounded(-1, kBound);
		ok &= expect(invalid_result.output.empty(), "invalid fd returns collected empty output");
		ok &= expect(!invalid_result.hit_limit, "invalid fd does not hit limit");

		std::array<ScopedFd, 2> small_pipe;
		ok &= expect(open_pipe(&small_pipe), "creates small read pipe");
		const std::string small_output = "small helper output\n";
		ok &= expect(howdy::native::write_all_to_fd(small_pipe[1].get(), small_output),
		             "writes small helper output");
		small_pipe[1].reset();
		const auto small_result =
		    howdy::native::read_fd_to_string_bounded(small_pipe[0].get(), kBound);
		ok &= expect(small_result.output == small_output, "reads complete small helper output");
		ok &= expect(!small_result.hit_limit, "small helper output does not hit limit");

		auto exact_file = create_temp_file(temp_root, "exact");
		ok &= expect(exact_file.has_value(), "creates exact-bounded input file");
		if (!exact_file.has_value()) {
			return false;
		}
		unlink(exact_file->path.c_str());
		ok &= expect(howdy::native::write_all_to_fd(exact_file->fd.get(), exact_output),
		             "writes exact bounded helper output");
		ok &= expect(lseek(exact_file->fd.get(), 0, SEEK_SET) == 0,
		             "rewinds exact bounded helper output");
		const auto exact_result =
		    howdy::native::read_fd_to_string_bounded(exact_file->fd.get(), kBound);
		ok &= expect(exact_result.output == exact_output, "reads exact bounded helper output");
		ok &= expect(exact_result.hit_limit, "exact bounded helper output hits limit");

		auto oversized_file = create_temp_file(temp_root, "oversized");
		ok &= expect(oversized_file.has_value(), "creates oversized input file");
		if (!oversized_file.has_value()) {
			return false;
		}
		unlink(oversized_file->path.c_str());
		ok &= expect(howdy::native::write_all_to_fd(oversized_file->fd.get(), oversized_output),
		             "writes oversized helper output");
		ok &= expect(lseek(oversized_file->fd.get(), 0, SEEK_SET) == 0,
		             "rewinds oversized helper output");
		const auto oversized_result =
		    howdy::native::read_fd_to_string_bounded(oversized_file->fd.get(), kBound);
		ok &= expect(oversized_result.output == oversized_output.substr(0, kBound),
		             "stops reading after bounded output threshold");
		ok &= expect(oversized_result.hit_limit, "oversized helper output hits limit");

		return ok;
	}

	auto expect_bounded_read_does_not_probe_open_pipe() -> bool {
		bool                    ok     = true;
		constexpr std::size_t   kBound = 1500;
		const std::string       exact_output(kBound, 'p');
		std::array<ScopedFd, 2> pipe_fds;
		ok &= expect(open_pipe(&pipe_fds), "creates exact-bound open pipe");
		if (!ok) {
			return false;
		}

		const pid_t child_pid = fork();
		ok &= expect(child_pid >= 0, "forks exact-bound pipe writer");
		if (child_pid < 0) {
			return false;
		}
		if (child_pid == 0) {
			pipe_fds[0].reset();
			const bool wrote = howdy::native::write_all_to_fd(pipe_fds[1].get(), exact_output);
			usleep(2000000);
			_exit(wrote ? 0 : 1);
		}

		pipe_fds[1].reset();
		const auto start   = std::chrono::steady_clock::now();
		const auto result  = howdy::native::read_fd_to_string_bounded(pipe_fds[0].get(), kBound);
		const auto elapsed = std::chrono::steady_clock::now() - start;

		ok &= expect(result.output == exact_output, "reads exact-bound open pipe output");
		ok &= expect(result.hit_limit, "exact-bound open pipe hits limit");
		ok &= expect(elapsed < std::chrono::milliseconds(1500),
		             "exact-bound open pipe read returns without blocking probe");

		kill(child_pid, SIGTERM);
		ok &= expect(wait_for_child(child_pid), "reaps exact-bound pipe writer");
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
	ok &= expect_bounded_read_does_not_probe_open_pipe();

	fs::remove_all(temp_root, ec);
	return ok ? 0 : 1;
}
