#include "support/fd_io.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/wait.h>

// Interpose only a synthetic descriptor; existing filesystem and pipe tests use real syscalls.
namespace {
	constexpr int kScriptedFd = 123456;

	struct ReadScript {
		std::string content;
		std::size_t offset       = 0;
		int         next_error   = 0;
		std::size_t error_offset = 0;
		std::size_t calls        = 0;
	};

	auto Script() -> ReadScript & {
		static ReadScript script;
		return script;
	}
}  // namespace

// GNU ld requires these exact symbol names for syscall interposition.
// NOLINTBEGIN(*reserved-identifier,cert-dcl*,readability-identifier-naming)
extern "C" auto __wrap_read(int fd, void *buffer, size_t count) -> ssize_t;
extern "C" auto __real_read(int fd, void *buffer, size_t count) -> ssize_t;
extern "C" auto __wrap___read_chk(int fd, void *buffer, size_t count, size_t buffer_size)
    -> ssize_t;

extern "C" auto __wrap_read(int fd, void *buffer, size_t count) -> ssize_t {
	if (fd != kScriptedFd) {
		return __real_read(fd, buffer, count);
	}
	++Script().calls;
	if (Script().next_error != 0 && Script().offset >= Script().error_offset) {
		errno = std::exchange(Script().next_error, 0);
		return -1;
	}
	const auto size = std::min(count, Script().content.size() - Script().offset);
	std::memcpy(buffer, std::string_view(Script().content).substr(Script().offset).data(), size);
	Script().offset += size;
	return static_cast<ssize_t>(size);
}

// glibc fixes this argument order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
extern "C" auto __wrap___read_chk(int fd, void *buffer, size_t count,
                                  [[maybe_unused]] size_t buffer_size) -> ssize_t {
	return __wrap_read(fd, buffer, count);
}

// NOLINTEND(*reserved-identifier,cert-dcl*,readability-identifier-naming)

namespace {

	using howdy::test::Expect;
	using howdy::test::ReadFile;

	using howdy::test::ScopedFd;

	struct TemporaryFile {
		std::filesystem::path path;
		ScopedFd              fd;
	};

	auto CreateTempFile(const std::filesystem::path &root, const std::string &label)
	    -> std::optional<TemporaryFile> {
		std::string       templ = (root / ("fd-io-" + label + "-XXXXXX")).string();
		std::vector<char> buffer(templ.begin(), templ.end());
		buffer.push_back('\0');

		ScopedFd fd(mkstemp(buffer.data()));
		if (fd.Get() < 0) {
			return std::nullopt;
		}

		return TemporaryFile{.path = buffer.data(), .fd = std::move(fd)};
	}

	auto ExpectEmptyWrite(const std::filesystem::path &temp_root) -> bool {
		bool ok   = true;
		auto file = CreateTempFile(temp_root, "empty");
		ok &= Expect(file.has_value(), "creates empty-write file");
		if (!file.has_value()) {
			return false;
		}

		const std::string empty;
		ok &= Expect(howdy::native::WriteAllToFd(file->fd.Get(), empty.data(), empty.size()),
		             "empty write succeeds");
		file->fd.Reset();
		ok &= Expect(ReadFile(file->path).empty(), "empty write leaves file empty");

		return ok;
	}

	auto ExpectInvalidFdFailure() -> bool {
		const std::string content = "x";
		return Expect(!howdy::native::WriteAllToFd(-1, content.data(), content.size()),
		              "invalid fd fails");
	}

	auto OpenPipe(std::array<ScopedFd, 2> *fds) -> bool {
		std::array<int, 2> raw_fds{{-1, -1}};
		if (pipe2(raw_fds.data(), O_CLOEXEC) != 0) {
			return false;
		}
		(*fds).at(0).Reset(raw_fds.at(0));
		(*fds).at(1).Reset(raw_fds.at(1));
		return true;
	}

	auto WaitForChild(pid_t child_pid) -> bool {
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

	auto ExpectBoundedReading(const std::filesystem::path &temp_root) -> bool {
		bool                    ok    = true;
		constexpr std::size_t   bound = 1500;
		const std::string       exact_output(bound, 'a');
		const std::string       oversized_output(4096, 'x');
		std::array<ScopedFd, 2> empty_pipe;
		ok &= Expect(OpenPipe(&empty_pipe), "creates empty read pipe");
		empty_pipe.at(1).Reset();
		const auto empty_result = howdy::native::ReadFdToStringBounded(
		    {.fd = empty_pipe.at(0).Get(), .max_bytes = bound});
		ok &= Expect(empty_result.output.empty(), "reads empty input");
		ok &= Expect(!empty_result.hit_limit, "empty input does not hit limit");
		ok &= Expect(!empty_result.read_error, "empty input does not report read error");
		ok &= Expect(empty_result.error_number == 0, "empty input leaves errno unset");
		const auto invalid_result =
		    howdy::native::ReadFdToStringBounded({.fd = -1, .max_bytes = bound});
		ok &= Expect(invalid_result.output.empty(), "invalid fd returns collected empty output");
		ok &= Expect(!invalid_result.hit_limit, "invalid fd does not hit limit");
		ok &= Expect(invalid_result.read_error, "invalid fd reports read error");
		ok &= Expect(invalid_result.error_number == EBADF, "invalid fd records EBADF");

		std::array<ScopedFd, 2> small_pipe;
		ok &= Expect(OpenPipe(&small_pipe), "creates small read pipe");
		const std::string small_output = "small helper output\n";
		ok &= Expect(howdy::native::WriteAllToFd(small_pipe.at(1).Get(), small_output),
		             "writes small helper output");
		small_pipe.at(1).Reset();
		const auto small_result = howdy::native::ReadFdToStringBounded(
		    {.fd = small_pipe.at(0).Get(), .max_bytes = bound});
		ok &= Expect(small_result.output == small_output, "reads complete small helper output");
		ok &= Expect(!small_result.hit_limit, "small helper output does not hit limit");
		ok &= Expect(!small_result.read_error, "small helper output does not report read error");
		ok &= Expect(small_result.error_number == 0, "small helper output leaves errno unset");

		auto exact_file = CreateTempFile(temp_root, "exact");
		ok &= Expect(exact_file.has_value(), "creates exact-bounded input file");
		if (!exact_file.has_value()) {
			return false;
		}
		unlink(exact_file->path.c_str());
		ok &= Expect(howdy::native::WriteAllToFd(exact_file->fd.Get(), exact_output),
		             "writes exact bounded helper output");
		ok &= Expect(lseek(exact_file->fd.Get(), 0, SEEK_SET) == 0,
		             "rewinds exact bounded helper output");
		const auto exact_result =
		    howdy::native::ReadFdToStringBounded({.fd = exact_file->fd.Get(), .max_bytes = bound});
		ok &= Expect(exact_result.output == exact_output, "reads exact bounded helper output");
		ok &= Expect(exact_result.hit_limit, "exact bounded helper output hits limit");

		auto oversized_file = CreateTempFile(temp_root, "oversized");
		ok &= Expect(oversized_file.has_value(), "creates oversized input file");
		if (!oversized_file.has_value()) {
			return false;
		}
		unlink(oversized_file->path.c_str());
		ok &= Expect(howdy::native::WriteAllToFd(oversized_file->fd.Get(), oversized_output),
		             "writes oversized helper output");
		ok &= Expect(lseek(oversized_file->fd.Get(), 0, SEEK_SET) == 0,
		             "rewinds oversized helper output");
		const auto oversized_result = howdy::native::ReadFdToStringBounded(
		    {.fd = oversized_file->fd.Get(), .max_bytes = bound});
		ok &= Expect(oversized_result.output == oversized_output.substr(0, bound),
		             "stops reading after bounded output threshold");
		ok &= Expect(oversized_result.hit_limit, "oversized helper output hits limit");

		return ok;
	}

	auto ExpectBoundedReadDoesNotProbeOpenPipe() -> bool {
		bool                    ok    = true;
		constexpr std::size_t   bound = 1500;
		const std::string       exact_output(bound, 'p');
		std::array<ScopedFd, 2> pipe_fds;
		ok &= Expect(OpenPipe(&pipe_fds), "creates exact-bound open pipe");
		if (!ok) {
			return false;
		}

		const pid_t child_pid = fork();
		ok &= Expect(child_pid >= 0, "forks exact-bound pipe writer");
		if (child_pid < 0) {
			return false;
		}
		if (child_pid == 0) {
			pipe_fds.at(0).Reset();
			const bool wrote = howdy::native::WriteAllToFd(pipe_fds.at(1).Get(), exact_output);
			usleep(2000000);
			_exit(wrote ? 0 : 1);
		}

		pipe_fds.at(1).Reset();
		const auto start = std::chrono::steady_clock::now();
		const auto result =
		    howdy::native::ReadFdToStringBounded({.fd = pipe_fds.at(0).Get(), .max_bytes = bound});
		const auto elapsed = std::chrono::steady_clock::now() - start;

		ok &= Expect(result.output == exact_output, "reads exact-bound open pipe output");
		ok &= Expect(result.hit_limit, "exact-bound open pipe hits limit");
		ok &= Expect(elapsed < std::chrono::milliseconds(1500),
		             "exact-bound open pipe read returns without blocking probe");

		kill(child_pid, SIGTERM);
		ok &= Expect(WaitForChild(child_pid), "reaps exact-bound pipe writer");
		return ok;
	}

	auto ExpectReadHintsAndInterruptions() -> bool {
		bool ok = true;
		for (const std::size_t hint :
		     {std::size_t{0}, std::size_t{1}, std::size_t{40000}, std::size_t{90000}}) {
			Script() = {
			    .content = std::string(40000, 'g'), .next_error = EINTR, .error_offset = 16384};
			const auto result = howdy::native::ReadFdToStringBounded(
			    {.fd = kScriptedFd, .max_bytes = 50000, .reserve_bytes = hint});
			ok &=
			    Expect(result.output == Script().content && !result.hit_limit && !result.read_error,
			           "reserve hint never truncates growing files or fabricates bytes after EOF; "
			           "EINTR retries");
			ok &= Expect(Script().calls == 5, "16 KiB chunks plus interrupt and EOF reads");
		}
		Script()        = {.content = "unread", .next_error = EIO};
		const auto zero = howdy::native::ReadFdToStringBounded({.fd = kScriptedFd, .max_bytes = 0});
		ok &= Expect(zero.output.empty() && !zero.hit_limit && !zero.read_error &&
		                 Script().calls == 0,
		             "zero bound performs no read");
		const auto failure =
		    howdy::native::ReadFdToStringBounded({.fd = kScriptedFd, .max_bytes = 100});
		ok &= Expect(failure.read_error && failure.error_number == EIO,
		             "non-EINTR read errors remain errors");
		Script() = {.content = std::string(40000, 'e'), .next_error = EIO, .error_offset = 16384};
		const auto partial =
		    howdy::native::ReadFdToStringBounded({.fd = kScriptedFd, .max_bytes = 50000});
		ok &= Expect(partial.output == Script().content.substr(0, 16384) && partial.read_error &&
		                 partial.error_number == EIO && !partial.hit_limit,
		             "read errors preserve bytes already collected");

		Script() = {.content = std::string(40000, 'b')};
		const auto bounded =
		    howdy::native::ReadFdToStringBounded({.fd = kScriptedFd, .max_bytes = 32768});
		ok &= Expect(bounded.output == Script().content.substr(0, 32768) && bounded.hit_limit &&
		                 Script().calls == 2,
		             "growing file stops at bound without probing EOF");
		return ok;
	}

	auto ExpectFullWrite(const std::filesystem::path &temp_root) -> bool {
		bool ok   = true;
		auto file = CreateTempFile(temp_root, "full");
		ok &= Expect(file.has_value(), "creates full-write file");
		if (!file.has_value()) {
			return false;
		}

		const std::string content = "alpha\nbeta\ngamma\n";
		ok &= Expect(howdy::native::WriteAllToFd(file->fd.Get(), content), "full write succeeds");
		file->fd.Reset();
		ok &= Expect(ReadFile(file->path) == content, "full write preserves content");

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
	ok &= Expect(!ec, "create temp root");

	ok &= ExpectEmptyWrite(temp_root);
	ok &= ExpectInvalidFdFailure();
	ok &= ExpectFullWrite(temp_root);
	ok &= ExpectBoundedReading(temp_root);
	ok &= ExpectReadHintsAndInterruptions();
	ok &= ExpectBoundedReadDoesNotProbeOpenPipe();

	fs::remove_all(temp_root, ec);
	return ok ? 0 : 1;
}
