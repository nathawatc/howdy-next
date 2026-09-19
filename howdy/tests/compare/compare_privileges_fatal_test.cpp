#include "compare/compare_privileges_test_groups.hpp"
#include "compare/compare_privileges_test_support.hpp"
#include "compare/privileges/internal.hpp"
#include "protocol/compare_exit.hpp"
#include "test_support.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/wait.h>

namespace {

	using howdy::native::CompareExit;
	using howdy::test::Expect;
	using howdy::test::compare_privileges::FakePrivilegeContext;
	using howdy::test::compare_privileges::MakeDependencies;
	using howdy::test::compare_privileges::SetNonRootIdentity;

	auto MarkerFd() -> int & {
		static int fd = -1;
		return fd;
	}

	void WriteMarker() {
		if (MarkerFd() >= 0) {
			const char    marker  = 'a';
			const ssize_t written = write(MarkerFd(), &marker, 1);
			(void)written;
		}
	}

	struct DestructorMarker {
		DestructorMarker()                                             = default;
		DestructorMarker(const DestructorMarker &)                     = delete;
		auto operator=(const DestructorMarker &) -> DestructorMarker & = delete;
		DestructorMarker(DestructorMarker &&)                          = delete;
		auto operator=(DestructorMarker &&) -> DestructorMarker &      = delete;

		~DestructorMarker() {
			if (MarkerFd() >= 0) {
				const char    marker  = 'd';
				const ssize_t written = write(MarkerFd(), &marker, 1);
				(void)written;
			}
		}
	};

	auto TestProductionNonRootIdentityFatalUsesExit() -> bool {
		std::array<int, 2> pipe_fds{};
		if (pipe2(pipe_fds.data(), O_CLOEXEC) != 0) {
			return Expect(false, "fatal regression pipe creation succeeds");
		}

		const pid_t child = fork();
		if (child < 0) {
			close(pipe_fds.at(0));
			close(pipe_fds.at(1));
			return Expect(false, "fatal regression fork succeeds");
		}
		if (child == 0) {
			close(pipe_fds.at(0));
			MarkerFd() = pipe_fds.at(1);
			(void)std::atexit(WriteMarker);
			DestructorMarker const marker;
			const int              null_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
			if (null_fd >= 0) {
				(void)dup2(null_fd, STDERR_FILENO);
				close(null_fd);
			}

			FakePrivilegeContext context;
			SetNonRootIdentity(context);
			context.uids.at(2) = 0;
			auto dependencies  = MakeDependencies(context);
			dependencies.fatal_exit =
			    howdy::native::compare_privileges_internal::FatalComparePrivilegeFailure;
			(void)howdy::native::compare_privileges_internal::DropComparePrivileges(dependencies);
			_exit(99);
		}

		close(pipe_fds.at(1));
		int status = 0;
		while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
		}
		std::array<char, 8> markers{};
		ssize_t             marker_count = 0;
		while (true) {
			const ssize_t count = read(pipe_fds.at(0), markers.data(), markers.size());
			if (count < 0 && errno == EINTR) {
				continue;
			}
			marker_count = count;
			break;
		}
		close(pipe_fds.at(0));

		bool ok = true;
		ok &= Expect(WIFEXITED(status), "fatal child exits normally through _exit");
		ok &= Expect(WEXITSTATUS(status) == static_cast<int>(CompareExit::kAbort),
		             "fatal child exits with CompareExit::kAbort");
		ok &= Expect(marker_count == 0, "fatal _exit skips destructor and atexit handlers");
		return ok;
	}

}  // namespace

auto RunComparePrivilegesFatalTests() -> bool {
	return TestProductionNonRootIdentityFatalUsesExit();
}
