#include "pam_test_support.hpp"
#include "prompt/internal_fd.hpp"
#include "prompt/native_prompt_conversation.hpp"
#include "prompt/native_prompt_test_groups.hpp"
#include "prompt/native_prompt_test_support.hpp"
#include "support/bit_flags.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <span>

#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include <security/pam_appl.h>

#include <sys/resource.h>
#include <sys/stat.h>

namespace {
	auto EligibilityConversation(int /*num_msg*/, const struct pam_message ** /*messages*/,
	                             struct pam_response **response, void * /*context*/) -> int {
		if (response != nullptr) {
			*response = nullptr;
		}
		return PAM_CONV_ERR;
	}

	struct NativeFdScenario {
		std::array<bool, 3> closed_stdio{};
		int                 terminal_stdio          = -1;
		bool                exhaust_tty_duplication = false;
	};

	auto RedirectStdioToNull(int null_fd) -> bool {
		return std::ranges::all_of(std::array{STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO},
		                           [null_fd](const int fd) -> bool {
			                           return dup2(null_fd, fd) == fd;
		                           });
	}

	auto CloseTestStdio(const std::array<bool, 3> &closed_stdio) -> bool {
		for (int fd = STDIN_FILENO; fd <= STDERR_FILENO; ++fd) {
			if (closed_stdio.at(static_cast<std::size_t>(fd)) && close(fd) != 0) {
				return false;
			}
		}
		return true;
	}

	auto DescriptorIsClosed(int fd) -> bool {
		errno = 0;
		return fcntl(fd, F_GETFD) == -1 && errno == EBADF;
	}

	struct DescriptorIdentity {
		bool        open  = false;
		int         flags = -1;
		int         error = 0;
		struct stat metadata{};
	};

	auto StandardDescriptorIdentities() -> std::array<DescriptorIdentity, 3> {
		std::array<DescriptorIdentity, 3> identities{};
		for (int fd = STDIN_FILENO; fd <= STDERR_FILENO; ++fd) {
			auto &identity = identities.at(static_cast<std::size_t>(fd));
			errno          = 0;
			identity.flags = fcntl(fd, F_GETFD);
			identity.error = identity.flags < 0 ? errno : 0;
			identity.open  = identity.flags >= 0 && fstat(fd, &identity.metadata) == 0;
		}
		return identities;
	}

	auto StandardDescriptorsMatch(const std::array<DescriptorIdentity, 3> &expected) -> bool {
		const auto actual = StandardDescriptorIdentities();
		for (std::size_t index = 0; index < expected.size(); ++index) {
			if (actual.at(index).open != expected.at(index).open) {
				return false;
			}
			if (!expected.at(index).open &&
			    (expected.at(index).error != EBADF || actual.at(index).error != EBADF)) {
				return false;
			}
			if (actual.at(index).open &&
			    (actual.at(index).flags != expected.at(index).flags ||
			     actual.at(index).metadata.st_dev != expected.at(index).metadata.st_dev ||
			     actual.at(index).metadata.st_ino != expected.at(index).metadata.st_ino ||
			     actual.at(index).metadata.st_rdev != expected.at(index).metadata.st_rdev)) {
				return false;
			}
		}
		return true;
	}

	auto TestInternalDescriptorsPreserveStdio(NativeFdScenario scenario) -> bool {
		ScopedFd master_fd;
		ScopedFd slave_fd;
		if (!OpenPtyPair(&master_fd, &slave_fd)) {
			return false;
		}
		const char *slave_name = ptsname(master_fd.Get());
		if (slave_name == nullptr || setsid() < 0) {
			return false;
		}
		(void)signal(SIGHUP, SIG_IGN);
		ScopedFd const terminal_fd(open(slave_name, O_RDWR | O_CLOEXEC));
		if (terminal_fd.Get() < 0 || tcsetpgrp(terminal_fd.Get(), getpgrp()) != 0) {
			return false;
		}

		ScopedFd const null_fd(open("/dev/null", O_RDWR | O_CLOEXEC));
		if (null_fd.Get() < 0) {
			return false;
		}
		if (!RedirectStdioToNull(null_fd.Get())) {
			return false;
		}
		if (scenario.terminal_stdio >= 0 &&
		    dup2(terminal_fd.Get(), scenario.terminal_stdio) != scenario.terminal_stdio) {
			return false;
		}
		struct stat terminal_before{};
		if (scenario.terminal_stdio >= 0 && fstat(scenario.terminal_stdio, &terminal_before) != 0) {
			return false;
		}

		const struct pam_conv original{.conv = EligibilityConversation, .appdata_ptr = nullptr};
		pam_handle_t         *pamh = nullptr;
		if (howdy::test::PamStartForTest("howdy-native-fd-test", "alice", &original, &pamh) !=
		    PAM_SUCCESS) {
			return false;
		}
		if (pam_set_item(pamh, PAM_TTY, slave_name) != PAM_SUCCESS) {
			pam_end(pamh, PAM_SYSTEM_ERR);
			return false;
		}
		if (!CloseTestStdio(scenario.closed_stdio)) {
			pam_end(pamh, PAM_SYSTEM_ERR);
			return false;
		}
		if (scenario.exhaust_tty_duplication) {
			const struct rlimit limit{.rlim_cur = STDERR_FILENO + 1, .rlim_max = STDERR_FILENO + 1};
			if (setrlimit(RLIMIT_NOFILE, &limit) != 0) {
				pam_end(pamh, PAM_SYSTEM_ERR);
				return false;
			}
		}
		const auto stdio_before = StandardDescriptorIdentities();

		NativePromptConversation const conversation(pamh);
		const bool                     available = conversation.Available();
		const std::array               internal_fds{
		    NativePromptConversationTestAccess::TtyFd(conversation),
		    NativePromptConversationTestAccess::AbortReadFd(conversation),
		    NativePromptConversationTestAccess::AbortWriteFd(conversation),
		};
		const bool  stdio_preserved = StandardDescriptorsMatch(stdio_before);
		struct stat terminal_after{};
		const bool  terminal_preserved =
		    scenario.terminal_stdio < 0 || (fstat(scenario.terminal_stdio, &terminal_after) == 0 &&
		                                    terminal_before.st_dev == terminal_after.st_dev &&
		                                    terminal_before.st_ino == terminal_after.st_ino &&
		                                    terminal_before.st_rdev == terminal_after.st_rdev);
		pam_end(pamh, PAM_SUCCESS);
		const bool expect_available =
		    scenario.terminal_stdio >= 0 && !scenario.exhaust_tty_duplication;
		const bool internal_fds_valid =
		    std::ranges::all_of(internal_fds,
		                        [](const int fd) -> bool {
			                        if (fd <= STDERR_FILENO) {
				                        return false;
			                        }
			                        const int descriptor_flags = fcntl(fd, F_GETFD);
			                        return descriptor_flags >= 0 &&
			                               howdy::native::HasAnyBits(descriptor_flags, FD_CLOEXEC);
		                        }) &&
		    internal_fds.at(0) != internal_fds.at(1) && internal_fds.at(0) != internal_fds.at(2) &&
		    internal_fds.at(1) != internal_fds.at(2);
		return available == expect_available && stdio_preserved && terminal_preserved &&
		       (!available || internal_fds_valid);
	}

	struct InternalFdFailureContext {
		std::size_t        fail_duplicate_call = 0;
		std::size_t        duplicate_calls     = 0;
		bool               fail_pipe           = false;
		std::array<int, 2> raw_pipe{{-1, -1}};
		std::array<int, 2> duplicated{{-1, -1}};
	};

	auto InjectedInternalDuplicate(void *context, int fd, int minimum_fd) -> int {
		auto &failure = *static_cast<InternalFdFailureContext *>(context);
		++failure.duplicate_calls;
		if (failure.duplicate_calls == failure.fail_duplicate_call) {
			errno = EMFILE;
			return -1;
		}
		const int duplicated = fcntl(fd, F_DUPFD_CLOEXEC, minimum_fd);
		if (failure.duplicate_calls <= failure.duplicated.size()) {
			failure.duplicated.at(failure.duplicate_calls - 1) = duplicated;
		}
		return duplicated;
	}

	auto InjectedInternalPipe(void *context, int *pipe_fds, int flags) -> int {
		auto &failure = *static_cast<InternalFdFailureContext *>(context);
		if (failure.fail_pipe) {
			errno = EMFILE;
			return -1;
		}
		const int result = pipe2(pipe_fds, flags);
		if (result == 0) {
			const auto descriptors = std::span(pipe_fds, std::size_t{2});
			failure.raw_pipe       = {descriptors.front(), descriptors.back()};
		}
		return result;
	}

	auto RecordedDescriptorsAreClosed(const InternalFdFailureContext &failure) -> bool {
		return std::ranges::all_of(std::array{failure.raw_pipe.at(0), failure.raw_pipe.at(1),
		                                      failure.duplicated.at(0), failure.duplicated.at(1)},
		                           [](const int fd) -> bool {
			                           return fd < 0 || DescriptorIsClosed(fd);
		                           });
	}

	auto InternalPipeFailureIsTransactional(std::size_t         fail_duplicate_call,
	                                        std::array<bool, 3> closed_stdio,
	                                        bool                fail_pipe = false) -> bool {
		if (!CloseTestStdio(closed_stdio)) {
			return false;
		}
		const auto               stdio_before = StandardDescriptorIdentities();
		InternalFdFailureContext context{
		    .fail_duplicate_call = fail_duplicate_call,
		    .fail_pipe           = fail_pipe,
		};
		const howdy::pam::detail::InternalFdOperations operations{
		    .context     = &context,
		    .duplicate   = InjectedInternalDuplicate,
		    .create_pipe = InjectedInternalPipe,
		};
		auto pipe = howdy::pam::detail::CreateInternalPipe(O_CLOEXEC | O_NONBLOCK, &operations);
		if (howdy::pam::detail::InternalPipeValid(pipe) || !RecordedDescriptorsAreClosed(context)) {
			return false;
		}
		if (!StandardDescriptorsMatch(stdio_before)) {
			return false;
		}
		auto conversation = CreateConversation({});
		return !conversation->Available();
	}
}  // namespace

auto ClosedStdinPreservedWithTerminalStdout() -> bool {
	return TestInternalDescriptorsPreserveStdio(
	    {.closed_stdio = {true, false, false}, .terminal_stdio = STDOUT_FILENO});
}

auto ClosedStdoutPreservedWithTerminalStdin() -> bool {
	return TestInternalDescriptorsPreserveStdio(
	    {.closed_stdio = {false, true, false}, .terminal_stdio = STDIN_FILENO});
}

auto ClosedStderrPreservedWithTerminalStdin() -> bool {
	return TestInternalDescriptorsPreserveStdio(
	    {.closed_stdio = {false, false, true}, .terminal_stdio = STDIN_FILENO});
}

auto ClosedStdinStdoutPreservedWithTerminalStderr() -> bool {
	return TestInternalDescriptorsPreserveStdio(
	    {.closed_stdio = {true, true, false}, .terminal_stdio = STDERR_FILENO});
}

auto ClosedAllStdioRemainClosed() -> bool {
	return TestInternalDescriptorsPreserveStdio(
	    {.closed_stdio = {true, true, true}, .terminal_stdio = -1});
}

auto TtyNormalizationFailurePreservesClosedStdin() -> bool {
	return TestInternalDescriptorsPreserveStdio({.closed_stdio            = {true, false, false},
	                                             .terminal_stdio          = STDOUT_FILENO,
	                                             .exhaust_tty_duplication = true});
}

auto AbortPipeCreationFailureIsTransactional() -> bool {
	return InternalPipeFailureIsTransactional(0, {}, true);
}

auto AbortPipeFirstNormalizationFailureIsTransactional() -> bool {
	return InternalPipeFailureIsTransactional(1, {true, false, false});
}

auto AbortPipeSecondNormalizationFailureIsTransactional() -> bool {
	return InternalPipeFailureIsTransactional(2, {true, true, false});
}
