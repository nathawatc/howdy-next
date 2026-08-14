#include "compare/compare_privileges_test_support.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

#include <sys/wait.h>

auto run_compare_privileges_fatal_tests() -> bool;

namespace {

	using howdy::native::CompareExit;
	using howdy::test::expect;
	using howdy::test::compare_privileges::FakePrivilegeContext;
	using howdy::test::compare_privileges::make_dependencies;
	using howdy::test::compare_privileges::set_non_root_identity;

	int marker_fd = -1;

	void write_marker() {
		if (marker_fd >= 0) {
			const char    marker  = 'a';
			const ssize_t written = write(marker_fd, &marker, 1);
			(void)written;
		}
	}

	struct DestructorMarker {
		~DestructorMarker() {
			if (marker_fd >= 0) {
				const char    marker  = 'd';
				const ssize_t written = write(marker_fd, &marker, 1);
				(void)written;
			}
		}
	};

	auto test_production_non_root_identity_fatal_uses_exit() -> bool {
		std::array<int, 2> pipe_fds{};
		if (pipe(pipe_fds.data()) != 0) {
			return expect(false, "fatal regression pipe creation succeeds");
		}

		const pid_t child = fork();
		if (child < 0) {
			close(pipe_fds[0]);
			close(pipe_fds[1]);
			return expect(false, "fatal regression fork succeeds");
		}
		if (child == 0) {
			close(pipe_fds[0]);
			marker_fd = pipe_fds[1];
			(void)std::atexit(write_marker);
			DestructorMarker marker;
			const int        null_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
			if (null_fd >= 0) {
				(void)dup2(null_fd, STDERR_FILENO);
				close(null_fd);
			}

			FakePrivilegeContext context;
			set_non_root_identity(context);
			context.uids[2]   = 0;
			auto dependencies = make_dependencies(context);
			dependencies.fatal_exit =
			    howdy::native::compare_privileges_internal::fatal_compare_privilege_failure;
			(void)howdy::native::compare_privileges_internal::drop_compare_privileges(dependencies);
			_exit(99);
		}

		close(pipe_fds[1]);
		int status = 0;
		while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
		}
		std::array<char, 8> markers{};
		ssize_t             marker_count = 0;
		while (true) {
			const ssize_t count = read(pipe_fds[0], markers.data(), markers.size());
			if (count < 0 && errno == EINTR) {
				continue;
			}
			marker_count = count;
			break;
		}
		close(pipe_fds[0]);

		bool ok = true;
		ok &= expect(WIFEXITED(status), "fatal child exits normally through _exit");
		ok &= expect(WEXITSTATUS(status) == static_cast<int>(CompareExit::kAbort),
		             "fatal child exits with CompareExit::kAbort");
		ok &= expect(marker_count == 0, "fatal _exit skips destructor and atexit handlers");
		return ok;
	}

}  // namespace

auto run_compare_privileges_fatal_tests() -> bool {
	return test_production_non_root_identity_fatal_uses_exit();
}
