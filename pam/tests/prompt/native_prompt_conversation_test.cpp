#include "prompt/internal_fd.hpp"
#include "prompt/native_prompt_conversation.hpp"
#include "prompt/native_prompt_input.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <future>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <termios.h>
#include <thread>
#include <unistd.h>

#include <security/pam_appl.h>

#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>

class NativePromptConversationTestAccess {
public:
	struct Descriptors {
		int tty_fd         = -1;
		int abort_read_fd  = -1;
		int abort_write_fd = -1;
	};

	struct Operations {
		void *context                                                  = nullptr;
		int (*poll_prompt)(void *, struct pollfd *, nfds_t, int)       = nullptr;
		ssize_t (*read_prompt)(void *, int, void *, std::size_t)       = nullptr;
		int (*restore_terminal)(void *, int, const struct termios *)   = nullptr;
		void (*post_message)(void *)                                   = nullptr;
		int (*set_pam_item)(void *, pam_handle_t *, int, const void *) = nullptr;
	};

	static auto create(Descriptors descriptors) -> std::unique_ptr<NativePromptConversation> {
		return create(descriptors, Operations{});
	}

	static auto create(Descriptors descriptors, Operations operations)
	    -> std::unique_ptr<NativePromptConversation> {
		auto production = NativePromptConversation::production_operations();
		NativePromptConversation::Operations injected{
		    .context = operations.context,
		    .poll_prompt =
		        operations.poll_prompt != nullptr ? operations.poll_prompt : production.poll_prompt,
		    .read_prompt =
		        operations.read_prompt != nullptr ? operations.read_prompt : production.read_prompt,
		    .restore_terminal = operations.restore_terminal != nullptr
		                            ? operations.restore_terminal
		                            : production.restore_terminal,
		    .post_message     = operations.post_message != nullptr ? operations.post_message
		                                                           : production.post_message,
		    .set_pam_item     = operations.set_pam_item != nullptr ? operations.set_pam_item
		                                                           : production.set_pam_item,
		};
		return std::unique_ptr<NativePromptConversation>(
		    new NativePromptConversation(nullptr, {}, true,
		                                 {.tty_fd         = descriptors.tty_fd,
		                                  .abort_read_fd  = descriptors.abort_read_fd,
		                                  .abort_write_fd = descriptors.abort_write_fd},
		                                 injected));
	}

	static auto dispatch(int num_msg, const struct pam_message **messages,
	                     struct pam_response **response, void *appdata_ptr) -> int {
		auto *conversation = static_cast<NativePromptConversation *>(appdata_ptr);
		return NativePromptConversation::dispatch(
		    num_msg, messages, response,
		    conversation == nullptr ? nullptr : conversation->dispatch_context_.get());
	}

	static auto prompt_input(NativePromptConversation &conversation,
	                         const struct pam_message &message, char **response, bool hide_input)
	    -> int {
		return conversation.prompt_input(message, response, hide_input);
	}

	[[nodiscard]] static auto terminal_restore_failed(const NativePromptConversation &conversation)
	    -> bool {
		return conversation.terminal_restore_failed_.load();
	}

	static void replace_descriptors(NativePromptConversation &conversation,
	                                Descriptors               descriptors) {
		conversation.tty_fd_     = descriptors.tty_fd;
		conversation.abort_pipe_ = {descriptors.abort_read_fd, descriptors.abort_write_fd};
	}

	static void close_abort_write_fd(NativePromptConversation &conversation) {
		::close(conversation.abort_pipe_[1]);
		conversation.abort_pipe_[1] = -1;
	}

	static void set_installed(NativePromptConversation &conversation, bool installed) {
		conversation.installed_ = installed;
	}

	static void set_pam_handle(NativePromptConversation &conversation, pam_handle_t *pamh) {
		conversation.pamh_ = pamh;
	}

	static auto override_conversation(const NativePromptConversation &conversation)
	    -> struct pam_conv {
		return conversation.override_conv_;

	}

	[[nodiscard]] static auto
	installed(const NativePromptConversation &conversation) -> bool {
		return conversation.installed_;
	}

	[[nodiscard]] static auto tty_fd(const NativePromptConversation &conversation) -> int {
		return conversation.tty_fd_;
	}

	[[nodiscard]] static auto abort_read_fd(const NativePromptConversation &conversation) -> int {
		return conversation.abort_pipe_[0];
	}

	[[nodiscard]] static auto abort_write_fd(const NativePromptConversation &conversation) -> int {
		return conversation.abort_pipe_[1];
	}
};

namespace {

	using howdy::test::expect;

	constexpr int kPromptReadTimeoutMs = 1000;

	struct OperationContext {
		NativePromptConversation *conversation        = nullptr;
		int                       poll_eintr_count    = 0;
		int                       read_eintr_count    = 0;
		int                       read_zero_count     = 0;
		int                       restore_eintr_count = 0;
		bool                      abort_on_poll       = false;
		bool                      abort_on_read       = false;
		bool                      restore_failure     = false;
		int                       throw_mode          = 0;
		std::array<int, 3>        pam_set_results{{PAM_SUCCESS, PAM_SUCCESS, PAM_SUCCESS}};
		int                       pam_set_calls = 0;
		struct pam_conv           last_pam_conversation{};
	};

	auto injected_poll(void *context, struct pollfd *fds, nfds_t count, int timeout) -> int {
		auto &operations = *static_cast<OperationContext *>(context);
		if (operations.poll_eintr_count > 0) {
			--operations.poll_eintr_count;
			if (operations.abort_on_poll) {
				operations.conversation->request_abort();
			}
			errno = EINTR;
			return -1;
		}
		return poll(fds, count, timeout);
	}

	auto injected_read(void *context, int fd, void *buffer, std::size_t count) -> ssize_t {
		auto &operations = *static_cast<OperationContext *>(context);
		if (operations.read_eintr_count > 0) {
			--operations.read_eintr_count;
			if (operations.abort_on_read) {
				operations.conversation->request_abort();
			}
			errno = EINTR;
			return -1;
		}
		if (operations.read_zero_count > 0) {
			--operations.read_zero_count;
			return 0;
		}
		return read(fd, buffer, count);
	}

	auto injected_restore(void *context, int fd, const struct termios *termios) -> int {
		auto &operations = *static_cast<OperationContext *>(context);
		if (operations.restore_eintr_count > 0) {
			--operations.restore_eintr_count;
			errno = EINTR;
			return -1;
		}
		if (operations.restore_failure) {
			errno = EIO;
			return -1;
		}
		return tcsetattr(fd, TCSANOW, termios);
	}

	void injected_post_message(void *context) {
		const auto &operations = *static_cast<OperationContext *>(context);
		if (operations.throw_mode == 1) {
			throw std::runtime_error("simulated dispatch failure");
		}
		if (operations.throw_mode == 2) {
			throw 1;
		}
	}

	auto injected_set_pam_item(void *context, pam_handle_t * /*pamh*/, int item_type,
	                           const void *item) -> int {
		auto &operations = *static_cast<OperationContext *>(context);
		if (item_type == PAM_CONV && item != nullptr) {
			operations.last_pam_conversation = *static_cast<const struct pam_conv *>(item);
		}
		const auto index = static_cast<std::size_t>(operations.pam_set_calls++);
		return index < operations.pam_set_results.size() ? operations.pam_set_results[index]
		                                                 : PAM_SYSTEM_ERR;
	}

	auto create_conversation(NativePromptConversationTestAccess::Descriptors descriptors,
	                         OperationContext                               *operations = nullptr)
	    -> std::unique_ptr<NativePromptConversation> {
		auto conversation = NativePromptConversationTestAccess::create(
		    descriptors, operations == nullptr ? NativePromptConversationTestAccess::Operations{}
		                                       : NativePromptConversationTestAccess::Operations{
		                                             .context          = operations,
		                                             .poll_prompt      = injected_poll,
		                                             .read_prompt      = injected_read,
		                                             .restore_terminal = injected_restore,
		                                             .post_message     = injected_post_message,
		                                             .set_pam_item     = injected_set_pam_item,
		                                         });
		if (operations != nullptr) {
			operations->conversation = conversation.get();
		}
		return conversation;
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

		[[nodiscard]] auto valid() const -> bool {
			return fd_ >= 0;
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

	auto open_pty_pair(ScopedFd *master_fd, ScopedFd *slave_fd) -> bool {
		master_fd->reset(posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC));
		if (!master_fd->valid()) {
			return false;
		}

		if (grantpt(master_fd->get()) != 0 || unlockpt(master_fd->get()) != 0) {
			master_fd->reset();
			return false;
		}

		char *slave_name = ptsname(master_fd->get());
		if (slave_name == nullptr) {
			master_fd->reset();
			return false;
		}

		slave_fd->reset(open(slave_name, O_RDWR | O_NOCTTY | O_CLOEXEC));
		if (!slave_fd->valid()) {
			master_fd->reset();
			return false;
		}

		return true;
	}

	auto open_pipe(std::array<ScopedFd, 2> *fds) -> bool {
		std::array<int, 2> raw_fds{{-1, -1}};
		if (pipe2(raw_fds.data(), O_CLOEXEC | O_NONBLOCK) != 0) {
			return false;
		}
		(*fds)[0].reset(raw_fds[0]);
		(*fds)[1].reset(raw_fds[1]);
		return true;
	}

	auto expect_isolated(bool (*scenario)(), const std::string &message) -> bool {
		const pid_t child_pid = fork();
		if (!expect(child_pid >= 0, message + ": child spawned")) {
			return false;
		}
		if (child_pid == 0) {
			_exit(scenario() ? EXIT_SUCCESS : EXIT_FAILURE);
		}

		int status = 0;
		for (int attempt = 0; attempt < 500; ++attempt) {
			const pid_t waited = waitpid(child_pid, &status, WNOHANG);
			if (waited == child_pid) {
				return expect(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS,
				              message + ": child succeeds");
			}
			if (waited < 0 && errno != EINTR) {
				return expect(false, message + ": waitpid failed, errno=" + std::to_string(errno));
			}
			(void)poll(nullptr, 0, 10);
		}

		(void)kill(child_pid, SIGTERM);
		for (int attempt = 0; attempt < 100; ++attempt) {
			const pid_t waited = waitpid(child_pid, &status, WNOHANG);
			if (waited == child_pid) {
				return expect(false, message + ": child exceeded timeout and was terminated");
			}
			if (waited < 0 && errno != EINTR) {
				return expect(
				    false, message + ": termination wait failed, errno=" + std::to_string(errno));
			}
			(void)poll(nullptr, 0, 10);
		}

		(void)kill(child_pid, SIGKILL);
		pid_t waited;
		do {
			waited = waitpid(child_pid, &status, 0);
		} while (waited < 0 && errno == EINTR);
		return expect(waited == child_pid, message + ": timed-out child reaped") &&
		       expect(false, message + ": child exceeded timeout and required SIGKILL");
	}

	auto eligibility_conversation(int /*num_msg*/, const struct pam_message ** /*messages*/,
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

	auto redirect_stdio_to_null(int null_fd) -> bool {
		return std::ranges::all_of(std::array{STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO},
		                           [null_fd](const int fd) -> bool {
			                           return dup2(null_fd, fd) == fd;
		                           });
	}

	auto close_test_stdio(const std::array<bool, 3> &closed_stdio) -> bool {
		for (int fd = STDIN_FILENO; fd <= STDERR_FILENO; ++fd) {
			if (closed_stdio[static_cast<std::size_t>(fd)] && close(fd) != 0) {
				return false;
			}
		}
		return true;
	}

	auto descriptor_is_closed(int fd) -> bool {
		errno = 0;
		return fcntl(fd, F_GETFD) == -1 && errno == EBADF;
	}

	struct DescriptorIdentity {
		bool        open  = false;
		int         flags = -1;
		int         error = 0;
		struct stat metadata{};
	};

	auto standard_descriptor_identities() -> std::array<DescriptorIdentity, 3> {
		std::array<DescriptorIdentity, 3> identities{};
		for (int fd = STDIN_FILENO; fd <= STDERR_FILENO; ++fd) {
			auto &identity = identities[static_cast<std::size_t>(fd)];
			errno          = 0;
			identity.flags = fcntl(fd, F_GETFD);
			identity.error = identity.flags < 0 ? errno : 0;
			identity.open  = identity.flags >= 0 && fstat(fd, &identity.metadata) == 0;
		}
		return identities;
	}

	auto standard_descriptors_match(const std::array<DescriptorIdentity, 3> &expected) -> bool {
		const auto actual = standard_descriptor_identities();
		for (std::size_t index = 0; index < expected.size(); ++index) {
			if (actual[index].open != expected[index].open) {
				return false;
			}
			if (!expected[index].open &&
			    (expected[index].error != EBADF || actual[index].error != EBADF)) {
				return false;
			}
			if (actual[index].open &&
			    (actual[index].flags != expected[index].flags ||
			     actual[index].metadata.st_dev != expected[index].metadata.st_dev ||
			     actual[index].metadata.st_ino != expected[index].metadata.st_ino ||
			     actual[index].metadata.st_rdev != expected[index].metadata.st_rdev)) {
				return false;
			}
		}
		return true;
	}

	auto test_internal_descriptors_preserve_stdio(NativeFdScenario scenario) -> bool {
		ScopedFd master_fd;
		ScopedFd slave_fd;
		if (!open_pty_pair(&master_fd, &slave_fd)) {
			return false;
		}
		const char *slave_name = ptsname(master_fd.get());
		if (slave_name == nullptr || setsid() < 0) {
			return false;
		}
		(void)signal(SIGHUP, SIG_IGN);
		ScopedFd terminal_fd(open(slave_name, O_RDWR | O_CLOEXEC));
		if (!terminal_fd.valid() || tcsetpgrp(terminal_fd.get(), getpgrp()) != 0) {
			return false;
		}

		ScopedFd null_fd(open("/dev/null", O_RDWR | O_CLOEXEC));
		if (!null_fd.valid()) {
			return false;
		}
		if (!redirect_stdio_to_null(null_fd.get())) {
			return false;
		}
		if (scenario.terminal_stdio >= 0 &&
		    dup2(terminal_fd.get(), scenario.terminal_stdio) != scenario.terminal_stdio) {
			return false;
		}
		struct stat terminal_before{};
		if (scenario.terminal_stdio >= 0 && fstat(scenario.terminal_stdio, &terminal_before) != 0) {
			return false;
		}

		const struct pam_conv original{.conv = eligibility_conversation, .appdata_ptr = nullptr};
		pam_handle_t         *pamh = nullptr;
		if (pam_start("howdy-native-fd-test", "alice", &original, &pamh) != PAM_SUCCESS) {
			return false;
		}
		if (pam_set_item(pamh, PAM_TTY, slave_name) != PAM_SUCCESS) {
			pam_end(pamh, PAM_SYSTEM_ERR);
			return false;
		}
		if (!close_test_stdio(scenario.closed_stdio)) {
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
		const auto stdio_before = standard_descriptor_identities();

		NativePromptConversation conversation(pamh);
		const bool               available = conversation.available();
		const std::array         internal_fds{
		    NativePromptConversationTestAccess::tty_fd(conversation),
		    NativePromptConversationTestAccess::abort_read_fd(conversation),
		    NativePromptConversationTestAccess::abort_write_fd(conversation),
		};
		const bool  stdio_preserved = standard_descriptors_match(stdio_before);
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
			                        return fd > STDERR_FILENO &&
			                               (fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0;
		                        }) &&
		    internal_fds[0] != internal_fds[1] && internal_fds[0] != internal_fds[2] &&
		    internal_fds[1] != internal_fds[2];
		return available == expect_available && stdio_preserved && terminal_preserved &&
		       (!available || internal_fds_valid);
	}

	auto closed_stdin_preserved_with_terminal_stdout() -> bool {
		return test_internal_descriptors_preserve_stdio(
		    {.closed_stdio = {true, false, false}, .terminal_stdio = STDOUT_FILENO});
	}

	auto closed_stdout_preserved_with_terminal_stdin() -> bool {
		return test_internal_descriptors_preserve_stdio(
		    {.closed_stdio = {false, true, false}, .terminal_stdio = STDIN_FILENO});
	}

	auto closed_stderr_preserved_with_terminal_stdin() -> bool {
		return test_internal_descriptors_preserve_stdio(
		    {.closed_stdio = {false, false, true}, .terminal_stdio = STDIN_FILENO});
	}

	auto closed_stdin_stdout_preserved_with_terminal_stderr() -> bool {
		return test_internal_descriptors_preserve_stdio(
		    {.closed_stdio = {true, true, false}, .terminal_stdio = STDERR_FILENO});
	}

	auto closed_all_stdio_remain_closed() -> bool {
		return test_internal_descriptors_preserve_stdio(
		    {.closed_stdio = {true, true, true}, .terminal_stdio = -1});
	}

	auto tty_normalization_failure_preserves_closed_stdin() -> bool {
		return test_internal_descriptors_preserve_stdio({.closed_stdio   = {true, false, false},
		                                                 .terminal_stdio = STDOUT_FILENO,
		                                                 .exhaust_tty_duplication = true});
	}

	struct InternalFdFailureContext {
		std::size_t        fail_duplicate_call = 0;
		std::size_t        duplicate_calls     = 0;
		bool               fail_pipe           = false;
		std::array<int, 2> raw_pipe{{-1, -1}};
		std::array<int, 2> duplicated{{-1, -1}};
	};

	auto injected_internal_duplicate(void *context, int fd, int minimum_fd) -> int {
		auto &failure = *static_cast<InternalFdFailureContext *>(context);
		++failure.duplicate_calls;
		if (failure.duplicate_calls == failure.fail_duplicate_call) {
			errno = EMFILE;
			return -1;
		}
		const int duplicated = fcntl(fd, F_DUPFD_CLOEXEC, minimum_fd);
		if (failure.duplicate_calls <= failure.duplicated.size()) {
			failure.duplicated[failure.duplicate_calls - 1] = duplicated;
		}
		return duplicated;
	}

	auto injected_internal_pipe(void *context, int *pipe_fds, int flags) -> int {
		auto &failure = *static_cast<InternalFdFailureContext *>(context);
		if (failure.fail_pipe) {
			errno = EMFILE;
			return -1;
		}
		const int result = pipe2(pipe_fds, flags);
		if (result == 0) {
			failure.raw_pipe = {pipe_fds[0], pipe_fds[1]};
		}
		return result;
	}

	auto recorded_descriptors_are_closed(const InternalFdFailureContext &failure) -> bool {
		return std::ranges::all_of(std::array{failure.raw_pipe[0], failure.raw_pipe[1],
		                                      failure.duplicated[0], failure.duplicated[1]},
		                           [](const int fd) -> bool {
			                           return fd < 0 || descriptor_is_closed(fd);
		                           });
	}

	auto internal_pipe_failure_is_transactional(std::size_t         fail_duplicate_call,
	                                            std::array<bool, 3> closed_stdio,
	                                            bool                fail_pipe = false) -> bool {
		if (!close_test_stdio(closed_stdio)) {
			return false;
		}
		const auto               stdio_before = standard_descriptor_identities();
		InternalFdFailureContext context{
		    .fail_duplicate_call = fail_duplicate_call,
		    .fail_pipe           = fail_pipe,
		};
		const howdy::pam::detail::InternalFdOperations operations{
		    .context     = &context,
		    .duplicate   = injected_internal_duplicate,
		    .create_pipe = injected_internal_pipe,
		};
		auto pipe = howdy::pam::detail::create_internal_pipe(O_CLOEXEC | O_NONBLOCK, &operations);
		if (pipe.valid() || !recorded_descriptors_are_closed(context)) {
			return false;
		}
		if (!standard_descriptors_match(stdio_before)) {
			return false;
		}
		auto conversation = create_conversation({});
		return !conversation->available();
	}

	auto abort_pipe_creation_failure_is_transactional() -> bool {
		return internal_pipe_failure_is_transactional(0, {}, true);
	}

	auto abort_pipe_first_normalization_failure_is_transactional() -> bool {
		return internal_pipe_failure_is_transactional(1, {true, false, false});
	}

	auto abort_pipe_second_normalization_failure_is_transactional() -> bool {
		return internal_pipe_failure_is_transactional(2, {true, true, false});
	}

	auto expect_native_terminal_eligibility() -> bool {
		ScopedFd                first_master;
		ScopedFd                first_slave;
		ScopedFd                second_master;
		ScopedFd                second_slave;
		std::array<ScopedFd, 2> pipes;
		bool                    ok = true;
		ok &= expect(open_pty_pair(&first_master, &first_slave),
		             "native eligibility opens interactive terminal");
		ok &= expect(open_pty_pair(&second_master, &second_slave),
		             "native eligibility opens mismatched terminal");
		ok &= expect(open_pipe(&pipes), "native eligibility opens graphical stdio substitute");
		if (!ok) {
			return false;
		}

		char *interactive_path = ptsname(first_master.get());
		if (!expect(interactive_path != nullptr,
		            "native eligibility resolves interactive terminal")) {
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
			const bool normal = native_prompt_terminal_is_interactive({.tty    = terminal_fd,
			                                                           .input  = terminal_fd,
			                                                           .output = terminal_fd,
			                                                           .error  = terminal_fd});
			const bool redirected_stdout =
			    native_prompt_terminal_is_interactive({.tty    = terminal_fd,
			                                           .input  = terminal_fd,
			                                           .output = pipes[1].get(),
			                                           .error  = terminal_fd});
			const bool stdout_only =
			    native_prompt_terminal_is_interactive({.tty    = terminal_fd,
			                                           .input  = pipes[0].get(),
			                                           .output = terminal_fd,
			                                           .error  = pipes[1].get()});
			const bool stderr_only =
			    native_prompt_terminal_is_interactive({.tty    = terminal_fd,
			                                           .input  = pipes[0].get(),
			                                           .output = pipes[1].get(),
			                                           .error  = terminal_fd});
			const bool unrelated_stdio =
			    native_prompt_terminal_is_interactive({.tty    = terminal_fd,
			                                           .input  = pipes[0].get(),
			                                           .output = pipes[1].get(),
			                                           .error  = pipes[1].get()});
			const bool regular_pam_tty =
			    native_prompt_terminal_is_interactive({.tty    = pipes[0].get(),
			                                           .input  = terminal_fd,
			                                           .output = terminal_fd,
			                                           .error  = terminal_fd});
			const unsigned failures = static_cast<unsigned>(!normal) |
			                          (static_cast<unsigned>(!redirected_stdout) << 1U) |
			                          (static_cast<unsigned>(!stdout_only) << 2U) |
			                          (static_cast<unsigned>(!stderr_only) << 3U) |
			                          (static_cast<unsigned>(unrelated_stdio) << 4U) |
			                          (static_cast<unsigned>(regular_pam_tty) << 5U);
			_exit(static_cast<int>(failures));
		}
		if (!expect(terminal_child > 0, "native eligibility spawns terminal child")) {
			return false;
		}
		int   terminal_status = 0;
		pid_t waited;
		do {
			waited = waitpid(terminal_child, &terminal_status, 0);
		} while (waited < 0 && errno == EINTR);
		ok &=
		    expect(waited == terminal_child, "native eligibility waits for terminal child, errno=" +
		                                         std::to_string(waited < 0 ? errno : 0));
		if (waited == terminal_child) {
			ok &= expect(WIFEXITED(terminal_status), "native eligibility child exits normally");
			if (WIFEXITED(terminal_status)) {
				const auto failures = static_cast<unsigned>(WEXITSTATUS(terminal_status));
				ok &= expect((failures & (1U << 0U)) == 0,
				             "all matching descriptors retain native mode");
				ok &= expect((failures & (1U << 1U)) == 0,
				             "redirected stdout with matching stdin/stderr retains native mode");
				ok &= expect((failures & (1U << 2U)) == 0,
				             "matching stdout with redirected stdin/stderr retains native mode");
				ok &= expect((failures & (1U << 3U)) == 0,
				             "matching stderr with redirected stdin/stdout retains native mode");
				ok &= expect((failures & (1U << 4U)) == 0,
				             "unrelated standard descriptors reject native mode for fallback");
				ok &= expect((failures & (1U << 5U)) == 0,
				             "regular-file PAM_TTY rejects native mode for fallback");
			}
		}
		ok &= expect(!native_prompt_terminal_is_interactive({.tty    = first_slave.get(),
		                                                     .input  = first_slave.get(),
		                                                     .output = first_slave.get(),
		                                                     .error  = first_slave.get()}),
		             "foreground mismatch rejects native mode for fallback");
		ok &= expect(!native_prompt_terminal_is_interactive({.tty    = first_slave.get(),
		                                                     .input  = second_slave.get(),
		                                                     .output = second_slave.get(),
		                                                     .error  = second_slave.get()}),
		             "different interactive descriptors reject native mode for fallback");
		ok &= expect(!native_prompt_terminal_is_interactive({.tty    = first_slave.get(),
		                                                     .input  = pipes[0].get(),
		                                                     .output = pipes[1].get(),
		                                                     .error  = pipes[1].get()}),
		             "GDM-like unrelated stdio rejects native mode for fallback");
		return ok;
	}

	auto expect_native_terminal_aliases() -> bool {
		ScopedFd first_master;
		ScopedFd first_slave;
		ScopedFd second_master;
		ScopedFd second_slave;
		bool     ok = true;
		ok &=
		    expect(open_pty_pair(&first_master, &first_slave), "native alias test opens first PTY");
		ok &= expect(open_pty_pair(&second_master, &second_slave),
		             "native alias test opens second PTY");
		if (!ok) {
			return false;
		}

		char             *first_name  = ptsname(first_master.get());
		const std::string first_path  = first_name == nullptr ? "" : first_name;
		char             *second_name = ptsname(second_master.get());
		const std::string second_path = second_name == nullptr ? "" : second_name;
		ok &= expect(first_name != nullptr && second_name != nullptr,
		             "native alias test resolves PTY paths");
		if (!ok) {
			return false;
		}

		const pid_t child = fork();
		if (child < 0) {
			return expect(false, "native alias test spawns terminal child");
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

			const bool path_target_alias = native_prompt_terminal_is_interactive(
			    {.tty = terminal_fd, .input = alias_fd, .output = -1, .error = -1});
			const bool alias_target_path = native_prompt_terminal_is_interactive(
			    {.tty = alias_fd, .input = terminal_fd, .output = -1, .error = -1});
			const bool different_pty_same_session = native_prompt_terminal_is_interactive(
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
		ok &= expect(waited == child, "native alias test verifies exact waitpid result");
		if (waited != child) {
			return ok;
		}
		ok &= expect(WIFEXITED(status), "native alias test child exits normally");
		if (!WIFEXITED(status)) {
			return ok;
		}
		const auto failures = static_cast<unsigned>(WEXITSTATUS(status));
		ok &= expect((failures & (1U << 0U)) == 0, "/dev/pts target accepts /dev/tty stdio alias");
		ok &= expect((failures & (1U << 1U)) == 0, "/dev/tty target accepts underlying PTY stdio");
		ok &= expect((failures & (1U << 2U)) == 0,
		             "different PTY in same process session is rejected");
		return ok;
	}

	struct ReadBuffer {
		char       *data = nullptr;
		std::size_t size = 0;
	};

	auto read_with_timeout(int fd, ReadBuffer buffer, int timeout_ms) -> ssize_t {
		struct pollfd poll_fd{.fd = fd, .events = POLLIN, .revents = 0};

		while (true) {
			const int poll_result = poll(&poll_fd, 1, timeout_ms);
			if (poll_result < 0 && errno == EINTR) {
				continue;
			}
			if (poll_result == 0) {
				return 0;
			}
			if (poll_result < 0 || (poll_fd.revents & POLLIN) == 0) {
				return -1;
			}

			while (true) {
				const ssize_t bytes_read = read(fd, buffer.data, buffer.size);
				if (bytes_read < 0 && errno == EINTR) {
					continue;
				}
				return bytes_read;
			}
		}
	}

	auto write_all(int fd, const char *data, std::size_t size) -> bool {
		std::size_t written = 0;
		while (written < size) {
			const ssize_t result = write(fd, data + written, size - written);
			if (result < 0 && errno == EINTR) {
				continue;
			}
			if (result <= 0) {
				return false;
			}
			written += static_cast<std::size_t>(result);
		}
		return true;
	}

	auto test_conv(int /*num_msg*/, const struct pam_message ** /*msgm*/,
	               struct pam_response **response, void *appdata_ptr) -> int {
		if (response != nullptr) {
			*response = nullptr;
		}
		return appdata_ptr == nullptr ? PAM_CONV_ERR : PAM_SUCCESS;
	}

	auto expect_dispatch_rejects_invalid_state() -> bool {
		bool ok = true;

		const struct pam_message message = {
		    .msg_style = PAM_TEXT_INFO,
		    .msg       = "notice",
		};
		const struct pam_message *message_ptr = &message;
		auto                     *responses   = reinterpret_cast<struct pam_response *>(0x1);

		ok &= expect(NativePromptConversationTestAccess::dispatch(1, &message_ptr, &responses,
		                                                          nullptr) == PAM_CONV_ERR,
		             "dispatch rejects null appdata");
		ok &= expect(responses == nullptr, "dispatch clears response on null appdata");
		ok &= expect(NativePromptConversationTestAccess::dispatch(1, &message_ptr, nullptr,
		                                                          nullptr) == PAM_CONV_ERR,
		             "dispatch rejects null response pointer");

		auto conversation = create_conversation({});
		responses         = reinterpret_cast<struct pam_response *>(0x1);
		ok &= expect(NativePromptConversationTestAccess::dispatch(
		                 0, &message_ptr, &responses, conversation.get()) == PAM_CONV_ERR,
		             "dispatch rejects zero message count");
		ok &= expect(responses == nullptr, "zero-message dispatch clears response");
		const struct pam_message *null_message = nullptr;
		responses                              = reinterpret_cast<struct pam_response *>(0x1);
		ok &= expect(NativePromptConversationTestAccess::dispatch(
		                 1, &null_message, &responses, conversation.get()) == PAM_CONV_ERR,
		             "dispatch rejects null message entry");
		ok &= expect(responses == nullptr, "dispatch clears response for null message entry");

		return ok;
	}

	auto expect_original_conversation_restored() -> bool {
		bool ok = true;

		ScopedFd                master_fd;
		ScopedFd                slave_fd;
		std::array<ScopedFd, 2> abort_pipe;
		ok &= expect(open_pty_pair(&master_fd, &slave_fd), "restore test opens pseudo terminal");
		ok &= expect(open_pipe(&abort_pipe), "restore test creates abort pipe");
		if (!ok) {
			return false;
		}

		int             appdata = 42;
		struct pam_conv original_conv{
		    .conv        = test_conv,
		    .appdata_ptr = &appdata,
		};
		pam_handle_t *pamh = nullptr;
		if (pam_start("howdy-native-test", "test-user", &original_conv, &pamh) != PAM_SUCCESS ||
		    pamh == nullptr) {
			return expect(false, "restore test starts PAM handle");
		}

		{
			NativePromptConversation conversation(pamh);
			NativePromptConversationTestAccess::replace_descriptors(
			    conversation, {.tty_fd         = slave_fd.release(),
			                   .abort_read_fd  = abort_pipe[0].release(),
			                   .abort_write_fd = abort_pipe[1].release()});
			ok &= expect(conversation.available(), "restore test native prompt is available");
			ok &= expect(conversation.install() == PAM_SUCCESS,
			             "restore test installs native conversation");
			conversation.restore_original();
		}

		const void *restored_item = nullptr;
		ok &= expect(pam_get_item(pamh, PAM_CONV, &restored_item) == PAM_SUCCESS,
		             "restore test reads PAM conversation");
		const auto *restored_conv = static_cast<const struct pam_conv *>(restored_item);
		ok &= expect(restored_conv != nullptr, "restore test returns restored PAM conversation");
		ok &= expect(restored_conv->conv == original_conv.conv,
		             "restore test restores original PAM conversation callback");
		ok &= expect(restored_conv->appdata_ptr == original_conv.appdata_ptr,
		             "restore test restores original PAM conversation appdata");
		pam_end(pamh, PAM_SUCCESS);
		return ok;
	}

	auto expect_invalid_pam_tty_is_unavailable() -> bool {
		int             appdata = 42;
		struct pam_conv original_conv{
		    .conv        = test_conv,
		    .appdata_ptr = &appdata,
		};
		pam_handle_t *pamh = nullptr;
		if (!expect(pam_start("howdy-native-tty-test", "test-user", &original_conv, &pamh) ==
		                PAM_SUCCESS,
		            "invalid PAM_TTY test starts PAM handle")) {
			return false;
		}

		bool ok = true;
		{
			NativePromptConversation conversation(pamh);
			ok &= expect(!conversation.available(), "missing PAM_TTY disables native prompt");
		}

		std::string path_template = "/tmp/howdy-pam-tty-XXXXXX";
		const int   regular_fd    = mkstemp(path_template.data());
		ok &= expect(regular_fd >= 0, "regular PAM_TTY test creates temporary file");
		if (regular_fd >= 0) {
			ok &= expect(pam_set_item(pamh, PAM_TTY, path_template.data()) == PAM_SUCCESS,
			             "regular PAM_TTY test sets PAM item");
			NativePromptConversation conversation(pamh);
			ok &= expect(!conversation.available(), "regular-file PAM_TTY disables native prompt");
			close(regular_fd);
			unlink(path_template.data());
		}
		pam_end(pamh, PAM_SUCCESS);
		return ok;
	}

	auto expect_abort_request_unblocks_without_pipe_wakeup() -> bool {
		bool ok = true;

		ScopedFd                master_fd;
		ScopedFd                slave_fd;
		std::array<ScopedFd, 2> abort_pipe;
		ok &= expect(open_pty_pair(&master_fd, &slave_fd), "abort wake test opens pseudo terminal");
		ok &= expect(open_pipe(&abort_pipe), "abort wake test creates abort pipe");
		if (!ok) {
			return false;
		}

		const struct pam_message message = {
		    .msg_style = PAM_PROMPT_ECHO_OFF,
		    .msg       = "Password: ",
		};

		OperationContext operations{.restore_failure = true};
		auto             conversation = std::shared_ptr<NativePromptConversation>(
		    create_conversation({.tty_fd         = slave_fd.release(),
		                         .abort_read_fd  = abort_pipe[0].release(),
		                         .abort_write_fd = abort_pipe[1].release()},
		                        &operations));
		NativePromptConversationTestAccess::close_abort_write_fd(*conversation);

		auto         response       = std::make_shared<char *>(nullptr);
		auto         result_promise = std::make_shared<std::promise<int>>();
		auto         result_future  = result_promise->get_future();
		std::jthread prompt_thread([conversation, message, response, result_promise] -> void {
			result_promise->set_value(NativePromptConversationTestAccess::prompt_input(
			    *conversation, message, response.get(), true));
		});

		std::array<char, 64> prompt_buffer{};
		const ssize_t        prompt_bytes = read_with_timeout(
		    master_fd.get(), {.data = prompt_buffer.data(), .size = prompt_buffer.size()},
		    kPromptReadTimeoutMs);
		ok &= expect(prompt_bytes > 0, "abort wake test prompt is written to tty");

		conversation->request_abort();
		if (result_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
			master_fd.reset();
			conversation->request_abort();
		}
		if (result_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
			(void)expect(false, "abort wake test prompt thread stops before timeout");
			_exit(EXIT_FAILURE);
		}
		conversation->request_abort();
		prompt_thread.join();

		const int prompt_result = result_future.get();
		ok &= expect(prompt_result == PAM_CONV_ERR,
		             "abort wake test terminal restore failure returns conversation error");
		ok &= expect(NativePromptConversationTestAccess::terminal_restore_failed(*conversation),
		             "abort wake test records terminal restore failure");
		ok &= expect(*response == nullptr, "abort wake test returns no response");
		if (*response != nullptr) {
			std::free(*response);
		}
		return ok;
	}

	auto expect_pty_hangup_aborts_prompt() -> bool {
		bool ok = true;

		ScopedFd                master_fd;
		ScopedFd                slave_fd;
		std::array<ScopedFd, 2> abort_pipe;
		ok &= expect(open_pty_pair(&master_fd, &slave_fd), "hangup test opens pseudo terminal");
		ok &= expect(open_pipe(&abort_pipe), "hangup test creates abort pipe");
		if (!ok) {
			return false;
		}

		const struct pam_message message = {
		    .msg_style = PAM_PROMPT_ECHO_OFF,
		    .msg       = "Password: ",
		};

		auto conversation = std::shared_ptr<NativePromptConversation>(
		    create_conversation({.tty_fd         = slave_fd.release(),
		                         .abort_read_fd  = abort_pipe[0].release(),
		                         .abort_write_fd = abort_pipe[1].release()}));

		auto         response       = std::make_shared<char *>(nullptr);
		auto         result_promise = std::make_shared<std::promise<int>>();
		auto         result_future  = result_promise->get_future();
		std::jthread prompt_thread([conversation, message, response, result_promise] -> void {
			result_promise->set_value(NativePromptConversationTestAccess::prompt_input(
			    *conversation, message, response.get(), true));
		});

		std::array<char, 64> prompt_buffer{};
		const ssize_t        prompt_bytes = read_with_timeout(
		    master_fd.get(), {.data = prompt_buffer.data(), .size = prompt_buffer.size()},
		    kPromptReadTimeoutMs);
		ok &= expect(prompt_bytes > 0, "hangup test prompt is written to tty");

		master_fd.reset();
		if (result_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
			conversation->request_abort();
		}
		if (result_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
			(void)expect(false, "PTY hangup prompt thread stops before timeout");
			_exit(EXIT_FAILURE);
		}
		conversation->request_abort();
		prompt_thread.join();

		const int prompt_result = result_future.get();
		ok &= expect(prompt_result == PAM_CONV_ERR, "PTY hangup aborts prompt");
		ok &= expect(*response == nullptr, "PTY hangup returns no response");
		if (*response != nullptr) {
			std::free(*response);
		}
		return ok;
	}

	auto expect_restore_handles_null_pam() -> bool {
		bool                    ok = true;
		ScopedFd                master_fd;
		ScopedFd                slave_fd;
		std::array<ScopedFd, 2> abort_pipe;

		ok &=
		    expect(open_pty_pair(&master_fd, &slave_fd), "null restore test opens pseudo terminal");
		ok &= expect(open_pipe(&abort_pipe), "null restore test creates abort pipe");
		if (!ok) {
			return false;
		}

		auto conversation = create_conversation({.tty_fd         = slave_fd.release(),
		                                         .abort_read_fd  = abort_pipe[0].release(),
		                                         .abort_write_fd = abort_pipe[1].release()});
		NativePromptConversationTestAccess::set_installed(*conversation, true);
		const auto result = conversation->restore_original();
		ok &= expect(!NativePromptConversationTestAccess::installed(*conversation),
		             "null PAM restore clears installed state");
		ok &= expect(result == howdy::pam::ConversationRestoreResult::kUnsafe,
		             "null PAM restore reports unsafe detachment");
		return ok;
	}

	auto expect_restore_result(std::array<int, 3>                    pam_set_results,
	                           howdy::pam::ConversationRestoreResult expected,
	                           const std::string                    &message) -> bool {
		OperationContext operations{.pam_set_results = pam_set_results};
		auto             conversation = create_conversation({}, &operations);
		NativePromptConversationTestAccess::set_pam_handle(*conversation,
		                                                   reinterpret_cast<pam_handle_t *>(0x1));
		NativePromptConversationTestAccess::set_installed(*conversation, true);
		const struct pam_conv override =
		    NativePromptConversationTestAccess::override_conversation(*conversation);
		const auto result = conversation->restore_original();
		bool       ok     = expect(result == expected, message + ": explicit restore result");
		const int  calls_before_destruction = operations.pam_set_calls;
		conversation.reset();
		ok &= expect(operations.pam_set_calls == calls_before_destruction,
		             message + ": destructor performs no PAM operation");

		if (expected == howdy::pam::ConversationRestoreResult::kUnsafe) {
			const struct pam_message  message_item{.msg_style = PAM_TEXT_INFO, .msg = "late"};
			const struct pam_message *message_ptr = &message_item;
			auto                     *response    = reinterpret_cast<struct pam_response *>(0x1);
			ok &= expect(override.conv(1, &message_ptr, &response, override.appdata_ptr) ==
			                 PAM_CONV_ERR,
			             message + ": retained callback fails closed after object destruction");
			ok &= expect(response == nullptr,
			             message + ": retained callback clears response after destruction");
		} else if (expected == howdy::pam::ConversationRestoreResult::kFailClosedInstalled) {
			const struct pam_message  message_item{.msg_style = PAM_TEXT_INFO, .msg = "late"};
			const struct pam_message *message_ptr = &message_item;
			auto                     *response    = reinterpret_cast<struct pam_response *>(0x1);
			ok &= expect(operations.last_pam_conversation.conv(
			                 1, &message_ptr, &response,
			                 operations.last_pam_conversation.appdata_ptr) == PAM_CONV_ERR,
			             message + ": installed static callback fails closed after destruction");
			ok &= expect(response == nullptr,
			             message + ": installed static callback clears response");
		}
		return ok;
	}

	auto expect_dispatch_throw_cleanup(int throw_mode, const std::string &message) -> bool {
		bool                    ok = true;
		ScopedFd                master_fd;
		ScopedFd                slave_fd;
		std::array<ScopedFd, 2> abort_pipe;

		ok &= expect(open_pty_pair(&master_fd, &slave_fd), message + ": opens pseudo terminal");
		ok &= expect(open_pipe(&abort_pipe), message + ": creates abort pipe");
		if (!ok) {
			return false;
		}

		OperationContext operations{.throw_mode = throw_mode};
		auto conversation = create_conversation({.tty_fd         = slave_fd.release(),
		                                         .abort_read_fd  = abort_pipe[0].release(),
		                                         .abort_write_fd = abort_pipe[1].release()},
		                                        &operations);

		const struct pam_message prompt = {
		    .msg_style = PAM_PROMPT_ECHO_OFF,
		    .msg       = "Password: ",
		};
		const struct pam_message *prompt_ptr      = &prompt;
		auto                     *responses       = reinterpret_cast<struct pam_response *>(0x1);
		int                       dispatch_result = PAM_SUCCESS;

		std::thread dispatch_thread([&] -> void {
			dispatch_result = NativePromptConversationTestAccess::dispatch(
			    1, &prompt_ptr, &responses, conversation.get());
		});

		std::array<char, 64> prompt_buffer{};
		const ssize_t        prompt_bytes = read_with_timeout(
		    master_fd.get(), {.data = prompt_buffer.data(), .size = prompt_buffer.size()},
		    kPromptReadTimeoutMs);
		ok &= expect(prompt_bytes > 0, message + ": prompt is written to tty");

		constexpr std::array<char, 7> kPassword{'s', 'e', 'c', 'r', 'e', 't', '\n'};
		ok &= expect(write(master_fd.get(), kPassword.data(), kPassword.size()) ==
		                 static_cast<ssize_t>(kPassword.size()),
		             message + ": writes password response");

		dispatch_thread.join();

		ok &= expect(dispatch_result == PAM_CONV_ERR, message + ": dispatch fails closed");
		ok &= expect(responses == nullptr, message + ": dispatch resets response");

		if (responses != nullptr) {
			std::free(responses);
		}
		return ok;
	}

	auto expect_prompt_input_returns_password_after_retry(NativePromptConversation *conversation,
	                                                      int master_fd, const std::string &message)
	    -> bool {
		bool ok = true;

		const struct pam_message prompt = {
		    .msg_style = PAM_PROMPT_ECHO_OFF,
		    .msg       = "Password: ",
		};

		int         prompt_result = PAM_SUCCESS;
		char       *response      = nullptr;
		std::thread prompt_thread([&] -> void {
			prompt_result = NativePromptConversationTestAccess::prompt_input(*conversation, prompt,
			                                                                 &response, true);
		});

		std::array<char, 64> prompt_buffer{};
		const ssize_t        prompt_bytes = read_with_timeout(
		    master_fd, {.data = prompt_buffer.data(), .size = prompt_buffer.size()},
		    kPromptReadTimeoutMs);
		ok &= expect(prompt_bytes > 0, message + ": prompt is written to tty");

		constexpr std::array<char, 7> kPassword{'s', 'e', 'c', 'r', 'e', 't', '\n'};
		ok &= expect(write(master_fd, kPassword.data(), kPassword.size()) ==
		                 static_cast<ssize_t>(kPassword.size()),
		             message + ": writes password response");

		prompt_thread.join();

		ok &= expect(prompt_result == PAM_SUCCESS, message + ": prompt succeeds after EINTR");
		ok &= expect(response != nullptr, message + ": prompt returns response");
		if (response != nullptr) {
			ok &= expect(std::string(response) == "secret", message + ": response matches input");
			std::free(response);
		}
		return ok;
	}

	auto expect_poll_eintr_without_abort_does_not_abort_prompt() -> bool {
		bool                    ok = true;
		ScopedFd                master_fd;
		ScopedFd                slave_fd;
		std::array<ScopedFd, 2> abort_pipe;

		ok &= expect(open_pty_pair(&master_fd, &slave_fd),
		             "poll EINTR retry test opens pseudo terminal");
		ok &= expect(open_pipe(&abort_pipe), "poll EINTR retry test creates abort pipe");
		if (!ok) {
			return false;
		}

		OperationContext operations{.poll_eintr_count = 1};
		auto conversation = create_conversation({.tty_fd         = slave_fd.release(),
		                                         .abort_read_fd  = abort_pipe[0].release(),
		                                         .abort_write_fd = abort_pipe[1].release()},
		                                        &operations);
		return expect_prompt_input_returns_password_after_retry(conversation.get(), master_fd.get(),
		                                                        "poll EINTR retry test");
	}

	auto expect_poll_eintr_with_abort_fails_closed() -> bool {
		bool                    ok = true;
		ScopedFd                master_fd;
		ScopedFd                slave_fd;
		std::array<ScopedFd, 2> abort_pipe;

		ok &= expect(open_pty_pair(&master_fd, &slave_fd),
		             "poll EINTR abort test opens pseudo terminal");
		ok &= expect(open_pipe(&abort_pipe), "poll EINTR abort test creates abort pipe");
		if (!ok) {
			return false;
		}

		const struct pam_message prompt = {
		    .msg_style = PAM_PROMPT_ECHO_OFF,
		    .msg       = "Password: ",
		};

		OperationContext operations{.poll_eintr_count = 1, .abort_on_poll = true};
		auto conversation = create_conversation({.tty_fd         = slave_fd.release(),
		                                         .abort_read_fd  = abort_pipe[0].release(),
		                                         .abort_write_fd = abort_pipe[1].release()},
		                                        &operations);

		int         prompt_result = PAM_SUCCESS;
		char       *response      = nullptr;
		std::thread prompt_thread([&] -> void {
			prompt_result = NativePromptConversationTestAccess::prompt_input(*conversation, prompt,
			                                                                 &response, true);
		});

		std::array<char, 64> prompt_buffer{};
		const ssize_t        prompt_bytes = read_with_timeout(
		    master_fd.get(), {.data = prompt_buffer.data(), .size = prompt_buffer.size()},
		    kPromptReadTimeoutMs);
		ok &= expect(prompt_bytes > 0, "poll EINTR abort test prompt is written to tty");

		prompt_thread.join();

		ok &= expect(prompt_result == PAM_CONV_ERR, "poll EINTR abort test fails closed");
		ok &= expect(response == nullptr, "poll EINTR abort test returns no response");
		if (response != nullptr) {
			std::free(response);
		}
		return ok;
	}

	auto expect_read_eintr_retries_and_accepts_input() -> bool {
		bool                    ok = true;
		ScopedFd                master_fd;
		ScopedFd                slave_fd;
		std::array<ScopedFd, 2> abort_pipe;

		ok &= expect(open_pty_pair(&master_fd, &slave_fd),
		             "read EINTR retry test opens pseudo terminal");
		ok &= expect(open_pipe(&abort_pipe), "read EINTR retry test creates abort pipe");
		if (!ok) {
			return false;
		}

		OperationContext operations{.read_eintr_count = 1};
		auto conversation = create_conversation({.tty_fd         = slave_fd.release(),
		                                         .abort_read_fd  = abort_pipe[0].release(),
		                                         .abort_write_fd = abort_pipe[1].release()},
		                                        &operations);
		return expect_prompt_input_returns_password_after_retry(conversation.get(), master_fd.get(),
		                                                        "read EINTR retry test");
	}

	auto expect_read_eintr_with_abort_fails_closed() -> bool {
		bool                    ok = true;
		ScopedFd                master_fd;
		ScopedFd                slave_fd;
		std::array<ScopedFd, 2> abort_pipe;

		ok &= expect(open_pty_pair(&master_fd, &slave_fd),
		             "read EINTR abort test opens pseudo terminal");
		ok &= expect(open_pipe(&abort_pipe), "read EINTR abort test creates abort pipe");
		if (!ok) {
			return false;
		}

		const struct pam_message prompt = {
		    .msg_style = PAM_PROMPT_ECHO_OFF,
		    .msg       = "Password: ",
		};

		OperationContext operations{.read_eintr_count = 1, .abort_on_read = true};
		auto conversation = create_conversation({.tty_fd         = slave_fd.release(),
		                                         .abort_read_fd  = abort_pipe[0].release(),
		                                         .abort_write_fd = abort_pipe[1].release()},
		                                        &operations);

		int         prompt_result = PAM_SUCCESS;
		char       *response      = nullptr;
		std::thread prompt_thread([&] -> void {
			prompt_result = NativePromptConversationTestAccess::prompt_input(*conversation, prompt,
			                                                                 &response, true);
		});

		std::array<char, 64> prompt_buffer{};
		const ssize_t        prompt_bytes = read_with_timeout(
		    master_fd.get(), {.data = prompt_buffer.data(), .size = prompt_buffer.size()},
		    kPromptReadTimeoutMs);
		ok &= expect(prompt_bytes > 0, "read EINTR abort test prompt is written to tty");

		constexpr char kPassword = 's';
		ok &= expect(write(master_fd.get(), &kPassword, 1) == 1,
		             "read EINTR abort test makes tty readable");

		prompt_thread.join();

		ok &= expect(prompt_result == PAM_CONV_ERR, "read EINTR abort test fails closed");
		ok &= expect(response == nullptr, "read EINTR abort test returns no response");
		if (response != nullptr) {
			std::free(response);
		}
		return ok;
	}

	auto expect_read_zero_retries_and_accepts_input() -> bool {
		bool                    ok = true;
		ScopedFd                master_fd;
		ScopedFd                slave_fd;
		std::array<ScopedFd, 2> abort_pipe;

		ok &= expect(open_pty_pair(&master_fd, &slave_fd),
		             "read zero retry test opens pseudo terminal");
		ok &= expect(open_pipe(&abort_pipe), "read zero retry test creates abort pipe");
		if (!ok) {
			return false;
		}

		OperationContext operations{.read_zero_count = 1};
		auto conversation = create_conversation({.tty_fd         = slave_fd.release(),
		                                         .abort_read_fd  = abort_pipe[0].release(),
		                                         .abort_write_fd = abort_pipe[1].release()},
		                                        &operations);
		return expect_prompt_input_returns_password_after_retry(conversation.get(), master_fd.get(),
		                                                        "read zero retry test");
	}

	auto expect_restore_eintr_retries_and_restores() -> bool {
		bool                    ok = true;
		ScopedFd                master_fd;
		ScopedFd                slave_fd;
		std::array<ScopedFd, 2> abort_pipe;

		ok &= expect(open_pty_pair(&master_fd, &slave_fd),
		             "restore EINTR retry test opens pseudo terminal");
		ok &= expect(open_pipe(&abort_pipe), "restore EINTR retry test creates abort pipe");
		if (!ok) {
			return false;
		}

		OperationContext operations{.restore_eintr_count = 1};
		auto conversation = create_conversation({.tty_fd         = slave_fd.release(),
		                                         .abort_read_fd  = abort_pipe[0].release(),
		                                         .abort_write_fd = abort_pipe[1].release()},
		                                        &operations);
		return expect_prompt_input_returns_password_after_retry(conversation.get(), master_fd.get(),
		                                                        "restore EINTR retry test");
	}

	auto expect_native_message_styles() -> bool {
		bool                    ok = true;
		ScopedFd                master_fd;
		ScopedFd                slave_fd;
		std::array<ScopedFd, 2> abort_pipe;
		ok &= expect(open_pty_pair(&master_fd, &slave_fd),
		             "message-style test opens pseudo terminal");
		ok &= expect(open_pipe(&abort_pipe), "message-style test creates abort pipe");
		if (!ok) {
			return false;
		}

		auto conversation = create_conversation({.tty_fd         = slave_fd.release(),
		                                         .abort_read_fd  = abort_pipe[0].release(),
		                                         .abort_write_fd = abort_pipe[1].release()});

		const struct pam_message echo_on_message{.msg_style = PAM_PROMPT_ECHO_ON, .msg = "Login: "};
		const struct pam_message *echo_on_ptr      = &echo_on_message;
		struct pam_response      *echo_on_response = nullptr;
		int                       echo_on_result   = PAM_CONV_ERR;
		std::thread               echo_on_thread([&] -> void {
			echo_on_result = NativePromptConversationTestAccess::dispatch(
			    1, &echo_on_ptr, &echo_on_response, conversation.get());
		});
		std::array<char, 64>      prompt_buffer{};
		ok &= expect(read_with_timeout(master_fd.get(),
		                               {.data = prompt_buffer.data(), .size = prompt_buffer.size()},
		                               kPromptReadTimeoutMs) > 0,
		             "message-style test writes echo-on prompt");
		constexpr std::string_view kVisibleInput = "visible\n";
		ok &= expect(write_all(master_fd.get(), kVisibleInput.data(), kVisibleInput.size()),
		             "message-style test writes echo-on response");
		echo_on_thread.join();
		ok &= expect(echo_on_result == PAM_SUCCESS, "PAM_PROMPT_ECHO_ON dispatch succeeds");
		ok &= expect(echo_on_response != nullptr &&
		                 std::string(echo_on_response[0].resp) == "visible",
		             "PAM_PROMPT_ECHO_ON returns visible input");
		if (echo_on_response != nullptr) {
			std::free(echo_on_response[0].resp);
			std::free(echo_on_response);
		}

		for (const int style : {PAM_TEXT_INFO, PAM_ERROR_MSG}) {
			const struct pam_message  message{.msg_style = style, .msg = "notice"};
			const struct pam_message *message_ptr = &message;
			struct pam_response      *responses   = nullptr;
			const int                 result      = NativePromptConversationTestAccess::dispatch(
			    1, &message_ptr, &responses, conversation.get());
			ok &= expect(result == PAM_SUCCESS,
			             "text and error message styles dispatch successfully");
			ok &= expect(responses != nullptr && responses[0].resp == nullptr,
			             "text and error message styles return empty responses");
			ok &= expect(
			    read_with_timeout(master_fd.get(),
			                      {.data = prompt_buffer.data(), .size = prompt_buffer.size()},
			                      kPromptReadTimeoutMs) > 0,
			    "text and error message styles write message lines");
			std::free(responses);
		}

		const struct pam_message  null_message{.msg_style = PAM_TEXT_INFO, .msg = nullptr};
		const struct pam_message *null_message_ptr = &null_message;
		struct pam_response      *null_responses   = nullptr;
		ok &= expect(NativePromptConversationTestAccess::dispatch(
		                 1, &null_message_ptr, &null_responses, conversation.get()) == PAM_SUCCESS,
		             "null message text is treated as empty text");
		ok &= expect(read_with_timeout(master_fd.get(),
		                               {.data = prompt_buffer.data(), .size = prompt_buffer.size()},
		                               kPromptReadTimeoutMs) > 0,
		             "null message text still writes newline");
		std::free(null_responses);

		const struct pam_message  unsupported_message{.msg_style = 99, .msg = "unsupported"};
		const struct pam_message *unsupported_ptr      = &unsupported_message;
		struct pam_response      *unsupported_response = nullptr;
		ok &= expect(
		    NativePromptConversationTestAccess::dispatch(1, &unsupported_ptr, &unsupported_response,
		                                                 conversation.get()) == PAM_CONV_ERR,
		    "unsupported message style is rejected");
		ok &= expect(unsupported_response == nullptr, "unsupported message style clears responses");

		auto                 invalid_tty          = create_conversation({});
		struct pam_response *invalid_tty_response = nullptr;
		ok &= expect(NativePromptConversationTestAccess::dispatch(
		                 1, &null_message_ptr, &invalid_tty_response, invalid_tty.get()) ==
		                 PAM_CONV_ERR,
		             "message dispatch rejects missing terminal");
		ok &= expect(invalid_tty_response == nullptr,
		             "missing terminal message dispatch clears responses");

		const int closed_tty = NativePromptConversationTestAccess::tty_fd(*conversation);
		close(closed_tty);
		const struct pam_message  write_failure_message{.msg_style = PAM_TEXT_INFO,
		                                                .msg       = "failure"};
		const struct pam_message *write_failure_ptr      = &write_failure_message;
		struct pam_response      *write_failure_response = nullptr;
		ok &= expect(NativePromptConversationTestAccess::dispatch(
		                 1, &write_failure_ptr, &write_failure_response, conversation.get()) ==
		                 PAM_CONV_ERR,
		             "message dispatch reports terminal write failure");
		ok &= expect(write_failure_response == nullptr, "terminal write failure clears responses");
		return ok;
	}

	auto expect_destroyed_installed_native_wrapper_fails_closed() -> bool {
		auto conversation = create_conversation({});
		NativePromptConversationTestAccess::set_installed(*conversation, true);
		const auto installed =
		    NativePromptConversationTestAccess::override_conversation(*conversation);
		conversation.reset();
		const struct pam_message  message{.msg_style = PAM_TEXT_INFO, .msg = "late"};
		const struct pam_message *message_ptr = &message;
		auto                     *response    = reinterpret_cast<struct pam_response *>(0x1);
		const int result = installed.conv(1, &message_ptr, &response, installed.appdata_ptr);
		return expect(result == PAM_CONV_ERR, "destroyed installed native wrapper fails closed") &&
		       expect(response == nullptr, "destroyed installed native wrapper clears response");
	}

	auto expect_native_prompt_input_edges() -> bool {
		using howdy::pam::native_prompt_input::CharacterResult;
		using howdy::pam::native_prompt_input::SensitiveBuffer;

		bool            ok = true;
		SensitiveBuffer password;
		bool            response_too_long = false;
		ok &= expect(howdy::pam::native_prompt_input::process_character(
		                 '\b', password, response_too_long) == CharacterResult::keep_reading &&
		                 password.empty(),
		             "backspace on empty password is ignored");
		ok &= expect(howdy::pam::native_prompt_input::process_character(
		                 'x', password, response_too_long) == CharacterResult::keep_reading &&
		                 password.size() == 1,
		             "ordinary character is appended to password");
		ok &= expect(howdy::pam::native_prompt_input::process_character(
		                 127, password, response_too_long) == CharacterResult::keep_reading &&
		                 password.empty(),
		             "delete removes last password character");
		response_too_long = true;
		ok &= expect(howdy::pam::native_prompt_input::process_character(
		                 'y', password, response_too_long) == CharacterResult::keep_reading &&
		                 password.empty(),
		             "characters after response overflow are drained");
		ok &= expect(howdy::pam::native_prompt_input::process_character(
		                 '\r', password, response_too_long) == CharacterResult::complete,
		             "carriage return completes password input");
		ok &= expect(howdy::pam::native_prompt_input::process_character(
		                 3, password, response_too_long) == CharacterResult::abort,
		             "Ctrl-C aborts password input");
		return ok;
	}

	auto expect_oversized_prompt_fails_closed() -> bool {
		bool                    ok = true;
		ScopedFd                master_fd;
		ScopedFd                slave_fd;
		std::array<ScopedFd, 2> abort_pipe;

		ok &= expect(open_pty_pair(&master_fd, &slave_fd),
		             "oversized prompt test opens pseudo terminal");
		ok &= expect(open_pipe(&abort_pipe), "oversized prompt test creates abort pipe");
		if (!ok) {
			return false;
		}

		const struct pam_message prompt = {
		    .msg_style = PAM_PROMPT_ECHO_OFF,
		    .msg       = "Password: ",
		};
		const int slave_raw_fd = slave_fd.get();
		auto      conversation = create_conversation({.tty_fd         = slave_fd.release(),
		                                              .abort_read_fd  = abort_pipe[0].release(),
		                                              .abort_write_fd = abort_pipe[1].release()});

		int         prompt_result = PAM_SUCCESS;
		char       *response      = nullptr;
		std::thread prompt_thread([&] -> void {
			prompt_result = NativePromptConversationTestAccess::prompt_input(*conversation, prompt,
			                                                                 &response, true);
		});

		std::array<char, 64> prompt_buffer{};
		const ssize_t        prompt_bytes = read_with_timeout(
		    master_fd.get(), {.data = prompt_buffer.data(), .size = prompt_buffer.size()},
		    kPromptReadTimeoutMs);
		ok &= expect(prompt_bytes > 0, "oversized prompt test writes prompt to tty");

		const std::string oversized_response = std::string(514, 'x') + "\n";
		ok &=
		    expect(write_all(master_fd.get(), oversized_response.data(), oversized_response.size()),
		           "oversized prompt test writes over-limit response");
		prompt_thread.join();

		ok &= expect(prompt_result == PAM_CONV_ERR,
		             "oversized prompt test rejects over-limit response");
		ok &= expect(response == nullptr, "oversized prompt test returns no response");
		struct termios restored_termios{};
		ok &= expect(tcgetattr(slave_raw_fd, &restored_termios) == 0,
		             "oversized prompt test can inspect restored terminal");
		ok &= expect((restored_termios.c_lflag & (ICANON | ECHO | ISIG)) == (ICANON | ECHO | ISIG),
		             "oversized prompt test restores terminal flags");
		if (response != nullptr) {
			std::free(response);
		}
		return ok;
	}

	auto expect_restore_failure_fails_closed() -> bool {
		bool                    ok = true;
		ScopedFd                master_fd;
		ScopedFd                slave_fd;
		std::array<ScopedFd, 2> abort_pipe;

		ok &= expect(open_pty_pair(&master_fd, &slave_fd),
		             "restore failure test opens pseudo terminal");
		ok &= expect(open_pipe(&abort_pipe), "restore failure test creates abort pipe");
		if (!ok) {
			return false;
		}

		const struct pam_message prompt = {
		    .msg_style = PAM_PROMPT_ECHO_OFF,
		    .msg       = "Password: ",
		};
		OperationContext operations{.restore_failure = true};
		auto conversation = create_conversation({.tty_fd         = slave_fd.release(),
		                                         .abort_read_fd  = abort_pipe[0].release(),
		                                         .abort_write_fd = abort_pipe[1].release()},
		                                        &operations);

		int         prompt_result = PAM_SUCCESS;
		char       *response      = nullptr;
		std::thread prompt_thread([&] -> void {
			prompt_result = NativePromptConversationTestAccess::prompt_input(*conversation, prompt,
			                                                                 &response, true);
		});

		std::array<char, 64> prompt_buffer{};
		const ssize_t        prompt_bytes = read_with_timeout(
		    master_fd.get(), {.data = prompt_buffer.data(), .size = prompt_buffer.size()},
		    kPromptReadTimeoutMs);
		ok &= expect(prompt_bytes > 0, "restore failure test writes prompt to tty");
		constexpr std::array<char, 7> kPassword{'s', 'e', 'c', 'r', 'e', 't', '\n'};
		ok &= expect(write_all(master_fd.get(), kPassword.data(), kPassword.size()),
		             "restore failure test writes password response");
		prompt_thread.join();

		ok &=
		    expect(prompt_result == PAM_CONV_ERR,
		           "restore failure test returns conversation error after terminal restore error");
		ok &= expect(NativePromptConversationTestAccess::terminal_restore_failed(*conversation),
		             "restore failure test records terminal restore failure");
		ok &= expect(response == nullptr, "restore failure test returns no response");
		if (response != nullptr) {
			std::free(response);
		}
		return ok;
	}

}  // namespace

auto main() -> int {
	bool ok = true;

	ScopedFd                master_fd;
	ScopedFd                slave_fd;
	std::array<ScopedFd, 2> abort_pipe;

	ok &= expect(open_pty_pair(&master_fd, &slave_fd), "opens pseudo terminal");
	ok &= expect(open_pipe(&abort_pipe), "creates abort pipe");
	if (!ok) {
		return 1;
	}

	const struct pam_message message = {
	    .msg_style = PAM_PROMPT_ECHO_OFF,
	    .msg       = "Password: ",
	};

	const int slave_raw_fd = slave_fd.get();
	auto      conversation = create_conversation({.tty_fd         = slave_fd.release(),
	                                              .abort_read_fd  = abort_pipe[0].release(),
	                                              .abort_write_fd = abort_pipe[1].release()});

	int         prompt_result = PAM_SUCCESS;
	char       *response      = nullptr;
	std::thread prompt_thread([&] -> void {
		prompt_result = NativePromptConversationTestAccess::prompt_input(*conversation, message,
		                                                                 &response, true);
	});

	std::array<char, 64> prompt_buffer{};
	const ssize_t        prompt_bytes = read_with_timeout(
	    master_fd.get(), {.data = prompt_buffer.data(), .size = prompt_buffer.size()},
	    kPromptReadTimeoutMs);
	ok &= expect(prompt_bytes > 0, "prompt is written to tty");

	constexpr char kCtrlC = 3;
	ok &= expect(write(master_fd.get(), &kCtrlC, 1) == 1, "writes Ctrl-C byte to pseudo terminal");

	prompt_thread.join();

	ok &= expect(prompt_result == PAM_CONV_ERR, "Ctrl-C byte aborts the native prompt");
	ok &= expect(!NativePromptConversationTestAccess::terminal_restore_failed(*conversation),
	             "successful terminal restoration keeps cancellation non-fatal");
	ok &= expect(response == nullptr, "aborted prompt does not return a response");

	struct termios restored_termios{};
	ok &= expect(tcgetattr(slave_raw_fd, &restored_termios) == 0,
	             "terminal state remains readable after abort");
	ok &= expect((restored_termios.c_lflag & ICANON) != 0, "canonical mode restored after abort");
	ok &= expect((restored_termios.c_lflag & ECHO) != 0, "echo restored after abort");
	ok &= expect((restored_termios.c_lflag & ISIG) != 0, "signal generation restored after abort");

	if (response != nullptr) {
		std::free(response);
	}

	ok &= expect_dispatch_rejects_invalid_state();
	ok &= expect_destroyed_installed_native_wrapper_fails_closed();
	ok &= expect_native_terminal_eligibility();
	ok &= expect_native_terminal_aliases();
	ok &= expect_isolated(closed_stdin_preserved_with_terminal_stdout,
	                      "closed stdin remains closed with terminal stdout");
	ok &= expect_isolated(closed_stdout_preserved_with_terminal_stdin,
	                      "closed stdout remains closed with terminal stdin");
	ok &= expect_isolated(closed_stderr_preserved_with_terminal_stdin,
	                      "closed stderr remains closed with terminal stdin");
	ok &= expect_isolated(closed_stdin_stdout_preserved_with_terminal_stderr,
	                      "closed stdin and stdout remain closed with terminal stderr");
	ok &= expect_isolated(closed_all_stdio_remain_closed,
	                      "all closed stdio remains closed and native stays unavailable");
	ok &= expect_isolated(tty_normalization_failure_preserves_closed_stdin,
	                      "PAM_TTY normalization failure preserves closed stdin");
	ok &= expect_isolated(abort_pipe_creation_failure_is_transactional,
	                      "abort pipe creation failure is transactional");
	ok &= expect_isolated(abort_pipe_first_normalization_failure_is_transactional,
	                      "abort pipe read normalization failure is transactional");
	ok &= expect_isolated(abort_pipe_second_normalization_failure_is_transactional,
	                      "abort pipe write normalization failure is transactional");
	ok &= expect_original_conversation_restored();
	ok &= expect_invalid_pam_tty_is_unavailable();
	ok &= expect_isolated(expect_abort_request_unblocks_without_pipe_wakeup, "abort wake test");
	ok &= expect_isolated(expect_pty_hangup_aborts_prompt, "PTY hangup test");
	ok &= expect_poll_eintr_without_abort_does_not_abort_prompt();
	ok &= expect_poll_eintr_with_abort_fails_closed();
	ok &= expect_read_eintr_retries_and_accepts_input();
	ok &= expect_read_eintr_with_abort_fails_closed();
	ok &= expect_read_zero_retries_and_accepts_input();
	ok &= expect_restore_eintr_retries_and_restores();
	ok &= expect_native_message_styles();
	ok &= expect_native_prompt_input_edges();
	ok &= expect_oversized_prompt_fails_closed();
	ok &= expect_restore_failure_fails_closed();
	ok &= expect_restore_handles_null_pam();
	ok &= expect_restore_result({PAM_SUCCESS, PAM_SUCCESS, PAM_SUCCESS},
	                            howdy::pam::ConversationRestoreResult::kOriginalRestored,
	                            "original conversation restoration");
	ok &= expect_restore_result({PAM_SYSTEM_ERR, PAM_SUCCESS, PAM_SUCCESS},
	                            howdy::pam::ConversationRestoreResult::kFailClosedInstalled,
	                            "fail-closed restoration fallback");
	ok &= expect_restore_result({PAM_SYSTEM_ERR, PAM_SYSTEM_ERR, PAM_SUCCESS},
	                            howdy::pam::ConversationRestoreResult::kUnsafe,
	                            "unsafe restoration fallback");
	ok &= expect_dispatch_throw_cleanup(1, "std exception after response allocation");
	ok &= expect_dispatch_throw_cleanup(2, "unknown exception after response allocation");

	return ok ? 0 : 1;
}
