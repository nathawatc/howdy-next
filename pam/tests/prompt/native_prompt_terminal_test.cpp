#include "prompt/native_prompt_test_support.hpp"
#include "test_support.hpp"

#include <cerrno>
#include <cstdlib>
#include <string>

#include <sys/wait.h>

using howdy::test::Expect;

auto ExpectNativeTerminalEligibility() -> bool;
auto ExpectNativeTerminalAliases() -> bool;
auto ExpectInvalidPamTtyIsUnavailable() -> bool;

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

	char *interactive_path = ptsname(first_master.Get());
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
		const bool normal            = NativePromptTerminalIsInteractive({.tty    = terminal_fd,
		                                                                  .input  = terminal_fd,
		                                                                  .output = terminal_fd,
		                                                                  .error  = terminal_fd});
		const bool redirected_stdout = NativePromptTerminalIsInteractive({.tty    = terminal_fd,
		                                                                  .input  = terminal_fd,
		                                                                  .output = pipes[1].Get(),
		                                                                  .error  = terminal_fd});
		const bool stdout_only       = NativePromptTerminalIsInteractive({.tty    = terminal_fd,
		                                                                  .input  = pipes[0].Get(),
		                                                                  .output = terminal_fd,
		                                                                  .error  = pipes[1].Get()});
		const bool stderr_only       = NativePromptTerminalIsInteractive({.tty    = terminal_fd,
		                                                                  .input  = pipes[0].Get(),
		                                                                  .output = pipes[1].Get(),
		                                                                  .error  = terminal_fd});
		const bool unrelated_stdio   = NativePromptTerminalIsInteractive({.tty    = terminal_fd,
		                                                                  .input  = pipes[0].Get(),
		                                                                  .output = pipes[1].Get(),
		                                                                  .error  = pipes[1].Get()});
		const bool regular_pam_tty   = NativePromptTerminalIsInteractive({.tty    = pipes[0].Get(),
		                                                                  .input  = terminal_fd,
		                                                                  .output = terminal_fd,
		                                                                  .error  = terminal_fd});
		const unsigned failures      = static_cast<unsigned>(!normal) |
		                               (static_cast<unsigned>(!redirected_stdout) << 1U) |
		                               (static_cast<unsigned>(!stdout_only) << 2U) |
		                               (static_cast<unsigned>(!stderr_only) << 3U) |
		                               (static_cast<unsigned>(unrelated_stdio) << 4U) |
		                               (static_cast<unsigned>(regular_pam_tty) << 5U);
		_exit(static_cast<int>(failures));
	}
	if (!Expect(terminal_child > 0, "native eligibility spawns terminal child")) {
		return false;
	}
	int   terminal_status = 0;
	pid_t waited;
	do {
		waited = waitpid(terminal_child, &terminal_status, 0);
	} while (waited < 0 && errno == EINTR);
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
	ok &= Expect(!NativePromptTerminalIsInteractive({.tty    = first_slave.Get(),
	                                                 .input  = first_slave.Get(),
	                                                 .output = first_slave.Get(),
	                                                 .error  = first_slave.Get()}),
	             "foreground mismatch rejects native mode for fallback");
	ok &= Expect(!NativePromptTerminalIsInteractive({.tty    = first_slave.Get(),
	                                                 .input  = second_slave.Get(),
	                                                 .output = second_slave.Get(),
	                                                 .error  = second_slave.Get()}),
	             "different interactive descriptors reject native mode for fallback");
	ok &= Expect(!NativePromptTerminalIsInteractive({.tty    = first_slave.Get(),
	                                                 .input  = pipes[0].Get(),
	                                                 .output = pipes[1].Get(),
	                                                 .error  = pipes[1].Get()}),
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

	char             *first_name  = ptsname(first_master.Get());
	const std::string first_path  = first_name == nullptr ? "" : first_name;
	char             *second_name = ptsname(second_master.Get());
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
		const unsigned failures = static_cast<unsigned>(!path_target_alias) |
		                          (static_cast<unsigned>(!alias_target_path) << 1U) |
		                          (static_cast<unsigned>(different_pty_same_session) << 2U);
		_exit(static_cast<int>(failures));
	}

	int   status = 0;
	pid_t waited;
	do {
		waited = waitpid(child, &status, 0);
	} while (waited < 0 && errno == EINTR);
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
	int             appdata = 42;
	struct pam_conv original_conv{
	    .conv        = NativePromptTerminalTestConv,
	    .appdata_ptr = &appdata,
	};
	pam_handle_t *pamh = nullptr;
	if (!Expect(pam_start("howdy-native-tty-test", "test-user", &original_conv, &pamh) ==
	                PAM_SUCCESS,
	            "invalid PAM_TTY test starts PAM handle")) {
		return false;
	}

	bool ok = true;
	{
		NativePromptConversation conversation(pamh);
		ok &= Expect(!conversation.Available(), "missing PAM_TTY disables native prompt");
	}

	std::string path_template = "/tmp/howdy-pam-tty-XXXXXX";
	const int   regular_fd    = mkstemp(path_template.data());
	ok &= Expect(regular_fd >= 0, "regular PAM_TTY test creates temporary file");
	if (regular_fd >= 0) {
		ok &= Expect(pam_set_item(pamh, PAM_TTY, path_template.data()) == PAM_SUCCESS,
		             "regular PAM_TTY test sets PAM item");
		NativePromptConversation conversation(pamh);
		ok &= Expect(!conversation.Available(), "regular-file PAM_TTY disables native prompt");
		close(regular_fd);
		unlink(path_template.data());
	}
	pam_end(pamh, PAM_SUCCESS);
	return ok;
}
