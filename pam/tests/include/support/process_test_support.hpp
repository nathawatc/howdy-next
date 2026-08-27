#pragma once

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <pthread.h>
#include <thread>
#include <unistd.h>
#include <utility>

#include <sys/wait.h>

namespace howdy::test::process {

	class ScopedSignalBlock {
	public:
		explicit ScopedSignalBlock(int signal_number) noexcept {
			sigset_t blocked{};
			if (sigemptyset(&blocked) != 0 || sigaddset(&blocked, signal_number) != 0) {
				return;
			}
			valid_ = pthread_sigmask(SIG_BLOCK, &blocked, &saved_mask_) == 0;
		}

		~ScopedSignalBlock() {
			if (valid_) {
				(void)pthread_sigmask(SIG_SETMASK, &saved_mask_, nullptr);
			}
		}

		ScopedSignalBlock(const ScopedSignalBlock &)                     = delete;
		auto operator=(const ScopedSignalBlock &) -> ScopedSignalBlock & = delete;

		[[nodiscard]] auto valid() const noexcept -> bool {
			return valid_;
		}

	private:
		sigset_t saved_mask_{};
		bool     valid_ = false;
	};

	inline auto reap_test_child(pid_t child_pid, int *status) -> bool {
		pid_t waited;
		do {
			waited = waitpid(child_pid, status, 0);
		} while (waited < 0 && errno == EINTR);
		if (waited == child_pid) {
			return true;
		}
		std::cerr << "waitpid(" << child_pid << ") failed: errno=" << errno << '\n';
		return false;
	}

	inline auto spawn_child(int exit_code, std::chrono::milliseconds delay = {}) -> pid_t {
		const pid_t child_pid = fork();
		if (child_pid == 0) {
			std::this_thread::sleep_for(delay);
			_exit(exit_code);
		}
		return child_pid;
	}

	inline auto spawn_exiting_child(int exit_code) -> pid_t {
		return spawn_child(exit_code);
	}

	inline auto spawn_signaled_child(int signal_number) -> pid_t {
		const pid_t child_pid = fork();
		if (child_pid == 0) {
			raise(signal_number);
			_exit(EXIT_FAILURE);
		}
		return child_pid;
	}

	inline auto spawn_blocked_child() -> pid_t {
		const pid_t child_pid = fork();
		if (child_pid == 0) {
			while (true) {
				pause();
			}
		}
		return child_pid;
	}

	inline void ignore_sigterm([[maybe_unused]] int signal_number) {}

	inline auto spawn_sigterm_ignoring_child() -> pid_t {
		std::array<int, 2> ready_pipe = {-1, -1};
		if (pipe(ready_pipe.data()) != 0) {
			return -1;
		}

		const pid_t child_pid = fork();
		if (child_pid == 0) {
			close(ready_pipe[0]);
			struct sigaction action = {};
			action.sa_handler       = ignore_sigterm;
			sigemptyset(&action.sa_mask);
			if (sigaction(SIGTERM, &action, nullptr) != 0) {
				_exit(EXIT_FAILURE);
			}
			const char ready = '1';
			ssize_t    write_result;
			do {
				write_result = write(ready_pipe[1], &ready, sizeof(ready));
			} while (write_result < 0 && errno == EINTR);
			if (std::cmp_not_equal(write_result, sizeof(ready))) {
				_exit(EXIT_FAILURE);
			}
			while (true) {
				pause();
			}
		}

		close(ready_pipe[1]);
		char ready = '\0';
		while (read(ready_pipe[0], &ready, 1) < 0 && errno == EINTR) {
		}
		close(ready_pipe[0]);
		if (child_pid <= 0 || ready != '1') {
			if (child_pid > 0) {
				(void)kill(child_pid, SIGKILL);
				(void)reap_test_child(child_pid, nullptr);
			}
			return -1;
		}
		return child_pid;
	}

	inline auto child_reaped(pid_t child_pid) -> bool {
		errno                   = 0;
		const pid_t wait_result = waitpid(child_pid, nullptr, WNOHANG);
		return wait_result == -1 && errno == ECHILD;
	}

}  // namespace howdy::test::process
