#include "pam_test_support.hpp"
#include "prompt/native_prompt_conversation.hpp"
#include "prompt/native_prompt_test_groups.hpp"
#include "prompt/native_prompt_test_support.hpp"
#include "support/eintr.hpp"
#include "test_support.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include <security/pam_appl.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

using howdy::test::Expect;

namespace {
	constexpr auto FailureBit(bool failed, unsigned shift) -> unsigned {
		return failed ? (1U << shift) : 0U;
	}
}  // namespace

auto ExpectNativeTerminalEligibility() -> bool {
	ScopedFd                first_master;
	ScopedFd                first_slave;
	ScopedFd                second_master;
	ScopedFd                second_slave;
	std::array<ScopedFd, 2> pipes;
	bool                    ok = true;
	ok &= Expect(OpenPtyPair(&first_master, &first_slave),
	             "native eligibility opens interactive terminal");
	ok &= Expect(OpenPtyPair(&second_master, &second_slave),
	             "native eligibility opens mismatched terminal");
	ok &= Expect(OpenPipe(&pipes), "native eligibility opens graphical stdio substitute");
	if (!ok) {
		return false;
	}

	char const *interactive_path = ptsname(first_master.Get());
	if (!Expect(interactive_path != nullptr, "native eligibility resolves interactive terminal")) {
		return false;
	}
	const pid_t terminal_child = fork();
	if (terminal_child == 0) {
		if (setsid() < 0) {
			_exit(EXIT_FAILURE);
		}
		const int terminal_fd = open(interactive_path, O_RDWR | O_CLOEXEC);
		if (terminal_fd < 0) {
			_exit(EXIT_FAILURE);
		}
		const bool     normal            = NativePromptTerminalIsInteractive({
		    .tty    = terminal_fd,
		    .input  = terminal_fd,
		    .output = terminal_fd,
		    .error  = terminal_fd,
		});
		const bool     redirected_stdout = NativePromptTerminalIsInteractive({
		    .tty    = terminal_fd,
		    .input  = terminal_fd,
		    .output = pipes.at(1).Get(),
		    .error  = terminal_fd,
		});
		const bool     stdout_only       = NativePromptTerminalIsInteractive({
		    .tty    = terminal_fd,
		    .input  = pipes.at(0).Get(),
		    .output = terminal_fd,
		    .error  = pipes.at(1).Get(),
		});
		const bool     stderr_only       = NativePromptTerminalIsInteractive({
		    .tty    = terminal_fd,
		    .input  = pipes.at(0).Get(),
		    .output = pipes.at(1).Get(),
		    .error  = terminal_fd,
		});
		const bool     unrelated_stdio   = NativePromptTerminalIsInteractive({
		    .tty    = terminal_fd,
		    .input  = pipes.at(0).Get(),
		    .output = pipes.at(1).Get(),
		    .error  = pipes.at(1).Get(),
		});
		const bool     regular_pam_tty   = NativePromptTerminalIsInteractive({
		    .tty    = pipes.at(0).Get(),
		    .input  = terminal_fd,
		    .output = terminal_fd,
		    .error  = terminal_fd,
		});
		const unsigned failures = FailureBit(!normal, 0U) | FailureBit(!redirected_stdout, 1U) |
		                          FailureBit(!stdout_only, 2U) | FailureBit(!stderr_only, 3U) |
		                          FailureBit(unrelated_stdio, 4U) | FailureBit(regular_pam_tty, 5U);
		_exit(static_cast<int>(failures));
	}
	if (!Expect(terminal_child > 0, "native eligibility spawns terminal child")) {
		return false;
	}
	int         terminal_status = 0;
	const pid_t waited          = howdy::native::RetryOnEintr([&] -> pid_t {
		return waitpid(terminal_child, &terminal_status, 0);
	});
	ok &= Expect(waited == terminal_child, "native eligibility waits for terminal child, errno=" +
	                                           std::to_string(waited < 0 ? errno : 0));
	if (waited == terminal_child) {
		ok &= Expect(WIFEXITED(terminal_status), "native eligibility child exits normally");
		if (WIFEXITED(terminal_status)) {
			const auto failures = static_cast<unsigned>(WEXITSTATUS(terminal_status));
			ok &=
			    Expect((failures & (1U << 0U)) == 0, "all matching descriptors retain native mode");
			ok &= Expect((failures & (1U << 1U)) == 0,
			             "redirected stdout with matching stdin/stderr retains native mode");
			ok &= Expect((failures & (1U << 2U)) == 0,
			             "matching stdout with redirected stdin/stderr retains native mode");
			ok &= Expect((failures & (1U << 3U)) == 0,
			             "matching stderr with redirected stdin/stdout retains native mode");
			ok &= Expect((failures & (1U << 4U)) == 0,
			             "unrelated standard descriptors reject native mode for fallback");
			ok &= Expect((failures & (1U << 5U)) == 0,
			             "regular-file PAM_TTY rejects native mode for fallback");
		}
	}
	ok &= Expect(!NativePromptTerminalIsInteractive({
	                 .tty    = first_slave.Get(),
	                 .input  = first_slave.Get(),
	                 .output = first_slave.Get(),
	                 .error  = first_slave.Get(),
	             }),
	             "foreground mismatch rejects native mode for fallback");
	ok &= Expect(!NativePromptTerminalIsInteractive({
	                 .tty    = first_slave.Get(),
	                 .input  = second_slave.Get(),
	                 .output = second_slave.Get(),
	                 .error  = second_slave.Get(),
	             }),
	             "different interactive descriptors reject native mode for fallback");
	ok &= Expect(!NativePromptTerminalIsInteractive({
	                 .tty    = first_slave.Get(),
	                 .input  = pipes.at(0).Get(),
	                 .output = pipes.at(1).Get(),
	                 .error  = pipes.at(1).Get(),
	             }),
	             "GDM-like unrelated stdio rejects native mode for fallback");
	return ok;
}

auto ExpectNativeTerminalAliases() -> bool {
	ScopedFd first_master;
	ScopedFd first_slave;
	ScopedFd second_master;
	ScopedFd second_slave;
	bool     ok = true;
	ok &= Expect(OpenPtyPair(&first_master, &first_slave), "native alias test opens first PTY");
	ok &= Expect(OpenPtyPair(&second_master, &second_slave), "native alias test opens second PTY");
	if (!ok) {
		return false;
	}

	char const       *first_name  = ptsname(first_master.Get());
	const std::string first_path  = first_name == nullptr ? "" : first_name;
	char const       *second_name = ptsname(second_master.Get());
	const std::string second_path = second_name == nullptr ? "" : second_name;
	ok &= Expect(first_name != nullptr && second_name != nullptr,
	             "native alias test resolves PTY paths");
	if (!ok) {
		return false;
	}

	const pid_t child = fork();
	if (child < 0) {
		return Expect(false, "native alias test spawns terminal child");
	}
	if (child == 0) {
		if (setsid() < 0) {
			_exit(EXIT_FAILURE);
		}
		const int terminal_fd = open(first_path.c_str(), O_RDWR | O_CLOEXEC);
		const int alias_fd    = open("/dev/tty", O_RDWR | O_CLOEXEC | O_NOCTTY);
		const int other_fd    = open(second_path.c_str(), O_RDWR | O_CLOEXEC | O_NOCTTY);
		if (terminal_fd < 0 || alias_fd < 0 || other_fd < 0) {
			_exit(EXIT_FAILURE);
		}

		const bool path_target_alias = NativePromptTerminalIsInteractive(
		    {.tty = terminal_fd, .input = alias_fd, .output = -1, .error = -1});
		const bool alias_target_path = NativePromptTerminalIsInteractive(
		    {.tty = alias_fd, .input = terminal_fd, .output = -1, .error = -1});
		const bool different_pty_same_session = NativePromptTerminalIsInteractive(
		    {.tty = terminal_fd, .input = other_fd, .output = -1, .error = -1});
		const unsigned failures = FailureBit(!path_target_alias, 0U) |
		                          FailureBit(!alias_target_path, 1U) |
		                          FailureBit(different_pty_same_session, 2U);
		_exit(static_cast<int>(failures));
	}

	int         status = 0;
	const pid_t waited = howdy::native::RetryOnEintr([&] -> pid_t {
		return waitpid(child, &status, 0);
	});
	ok &= Expect(waited == child, "native alias test verifies exact waitpid result");
	if (waited != child) {
		return ok;
	}
	ok &= Expect(WIFEXITED(status), "native alias test child exits normally");
	if (!WIFEXITED(status)) {
		return ok;
	}
	const auto failures = static_cast<unsigned>(WEXITSTATUS(status));
	ok &= Expect((failures & (1U << 0U)) == 0, "/dev/pts target accepts /dev/tty stdio alias");
	ok &= Expect((failures & (1U << 1U)) == 0, "/dev/tty target accepts underlying PTY stdio");
	ok &= Expect((failures & (1U << 2U)) == 0, "different PTY in same process session is rejected");
	return ok;
}

namespace {
	auto NativePromptTerminalTestConv(int /*num_msg*/, const struct pam_message ** /*msgm*/,
	                                  struct pam_response **response, void *appdata_ptr) -> int {
		if (response != nullptr) {
			*response = nullptr;
		}
		return appdata_ptr == nullptr ? PAM_CONV_ERR : PAM_SUCCESS;
	}
}  // namespace

auto ExpectInvalidPamTtyIsUnavailable() -> bool {
	int                   appdata = 42;
	struct pam_conv const original_conv{
	    .conv        = NativePromptTerminalTestConv,
	    .appdata_ptr = &appdata,
	};
	pam_handle_t *pamh = nullptr;
	if (!Expect(howdy::test::PamStartForTest("howdy-native-tty-test", "test-user", &original_conv,
	                                         &pamh) == PAM_SUCCESS,
	            "invalid PAM_TTY test starts PAM handle")) {
		return false;
	}

	bool ok = true;
	{
		NativePromptConversation const conversation(pamh);
		ok &= Expect(!conversation.Available(), "missing PAM_TTY disables native prompt");
	}

	std::string path_template = "/tmp/howdy-pam-tty-XXXXXX";
	const int   regular_fd    = mkstemp(path_template.data());
	ok &= Expect(regular_fd >= 0, "regular PAM_TTY test creates temporary file");
	if (regular_fd >= 0) {
		ok &= Expect(pam_set_item(pamh, PAM_TTY, path_template.data()) == PAM_SUCCESS,
		             "regular PAM_TTY test sets PAM item");
		NativePromptConversation const conversation(pamh);
		ok &= Expect(!conversation.Available(), "regular-file PAM_TTY disables native prompt");
		close(regular_fd);
		unlink(path_template.data());
	}
	pam_end(pamh, PAM_SUCCESS);
	return ok;
}

auto ExpectPamTtyResolution() -> bool {
	bool       ok          = true;
	const auto resolves_to = [](std::string_view pam_tty, std::string_view expected) -> bool {
		const auto target = ResolveNativePromptTty(pam_tty);
		return target.has_value() && target->path == expected;
	};

	ok &= Expect(!ResolveNativePromptTty("").has_value(), "empty PAM_TTY is rejected");
	ok &= Expect(resolves_to("/dev/null", "/dev/null"), "absolute /dev character device resolves");
	ok &= Expect(resolves_to("null", "/dev/null"), "relative PAM_TTY resolves under /dev");
	ok &= Expect(resolves_to("/dev/../dev/null", "/dev/null"),
	             "dot-dot staying in /dev canonicalizes");
	ok &= Expect(!ResolveNativePromptTty("/dev/../etc/passwd").has_value(),
	             "dot-dot escaping /dev is rejected");
	ok &=
	    Expect(!ResolveNativePromptTty("/etc/passwd").has_value(), "path outside /dev is rejected");
	ok &=
	    Expect(!ResolveNativePromptTty("/dev/shm").has_value(), "directory under /dev is rejected");

	{
		howdy::test::TtySymlink into_dev;
		ok &=
		    Expect(into_dev.Create("/dev/null") != nullptr, "PAM_TTY test creates symlink to /dev");
		ok &= Expect(resolves_to(into_dev.Path(), "/dev/null"),
		             "symlink outside /dev resolves to its canonical /dev target");
		howdy::test::TtySymlink out_of_dev;
		ok &= Expect(out_of_dev.Create("/etc/passwd") != nullptr,
		             "PAM_TTY test creates symlink outside /dev");
		ok &= Expect(!ResolveNativePromptTty(out_of_dev.Path()).has_value(),
		             "symlink resolving outside /dev is rejected");
	}

	{
		ScopedFd master;
		ScopedFd slave;
		ok &= Expect(OpenPtyPair(&master, &slave), "PAM_TTY test opens pseudo-terminal");
		std::array<char, 128> slave_name{};
		ok &= Expect(ttyname_r(slave.Get(), slave_name.data(), slave_name.size()) == 0,
		             "PAM_TTY test names pseudo-terminal slave");
		const auto  target = ResolveNativePromptTty(slave_name.data());
		struct stat slave_metadata{};
		ok &= Expect(target.has_value() && target->path == slave_name.data() &&
		                 fstat(slave.Get(), &slave_metadata) == 0 &&
		                 target->rdevice == slave_metadata.st_rdev,
		             "pseudo-terminal slave resolves to its character device");
	}

	// /dev/shm is world-writable and starts with /dev/; a FIFO there must not be opened.
	const std::string fifo_path = "/dev/shm/howdy-pam-tty-fifo-" + std::to_string(getpid());
	unlink(fifo_path.c_str());
	if (mkfifo(fifo_path.c_str(), 0600) == 0) {
		ok &= Expect(!ResolveNativePromptTty(fifo_path).has_value(),
		             "non-character node under /dev is rejected");
		unlink(fifo_path.c_str());
	} else {
		const int error = errno;
		ok &= Expect(error == ENOENT || error == EACCES || error == EROFS,
		             "PAM_TTY FIFO setup fails only when /dev/shm is unavailable");
		std::cerr << "SKIP: /dev/shm FIFO PAM_TTY case (" << std::strerror(error) << ")\n";
	}
	return ok;
}
