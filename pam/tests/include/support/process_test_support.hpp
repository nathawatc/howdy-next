#pragma once

#include "support/eintr.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/wait.h>

namespace howdy::test::process {

	class ScopedSignalBlock {
	public:
		ScopedSignalBlock(ScopedSignalBlock &&)                     = delete;
		auto operator=(ScopedSignalBlock &&) -> ScopedSignalBlock & = delete;

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

		[[nodiscard]] auto Valid() const noexcept -> bool {
			return valid_;
		}

	private:
		sigset_t saved_mask_{};
		bool     valid_ = false;
	};

	inline auto ReapTestChild(pid_t child_pid, int *status) -> bool {
		const pid_t waited = howdy::native::RetryOnEintr([&] -> pid_t {
			return waitpid(child_pid, status, 0);
		});
		if (waited == child_pid) {
			return true;
		}
		std::cerr << "waitpid(" << child_pid << ") failed: errno=" << errno << '\n';
		return false;
	}

	inline auto SpawnChild(int exit_code, std::chrono::milliseconds delay = {}) -> pid_t {
		const pid_t child_pid = fork();
		if (child_pid == 0) {
			std::this_thread::sleep_for(delay);
			_exit(exit_code);
		}
		return child_pid;
	}

	inline auto SpawnExitingChild(int exit_code) -> pid_t {
		return SpawnChild(exit_code);
	}

	inline auto SpawnSignaledChild(int signal_number) -> pid_t {
		const pid_t child_pid = fork();
		if (child_pid == 0) {
			if (raise(signal_number) != 0) {
				_exit(EXIT_FAILURE);
			}
			_exit(EXIT_FAILURE);
		}
		return child_pid;
	}

	inline auto SpawnBlockedChild() -> pid_t {
		const pid_t child_pid = fork();
		if (child_pid == 0) {
			while (true) {
				pause();
			}
		}
		return child_pid;
	}

	inline void IgnoreSigterm([[maybe_unused]] int signal_number) {}

	inline auto SpawnSigtermIgnoringChild() -> pid_t {
		std::array<int, 2> ready_pipe = {-1, -1};
		if (pipe2(ready_pipe.data(), O_CLOEXEC) != 0) {
			return -1;
		}

		const pid_t child_pid = fork();
		if (child_pid == 0) {
			close(ready_pipe.at(0));
			struct sigaction action = {};
			action.sa_handler       = IgnoreSigterm;
			sigemptyset(&action.sa_mask);
			if (sigaction(SIGTERM, &action, nullptr) != 0) {
				_exit(EXIT_FAILURE);
			}
			const char    ready        = '1';
			const ssize_t write_result = howdy::native::RetryOnEintr([&] -> ssize_t {
				return write(ready_pipe.at(1), &ready, sizeof(ready));
			});
			if (std::cmp_not_equal(write_result, sizeof(ready))) {
				_exit(EXIT_FAILURE);
			}
			while (true) {
				pause();
			}
		}

		close(ready_pipe.at(1));
		char ready = '\0';
		while (read(ready_pipe.at(0), &ready, 1) < 0 && errno == EINTR) {
		}
		close(ready_pipe.at(0));
		if (child_pid <= 0 || ready != '1') {
			if (child_pid > 0) {
				(void)kill(child_pid, SIGKILL);
				(void)ReapTestChild(child_pid, nullptr);
			}
			return -1;
		}
		return child_pid;
	}

	inline auto ChildReaped(pid_t child_pid) -> bool {
		errno                   = 0;
		const pid_t wait_result = waitpid(child_pid, nullptr, WNOHANG);
		return wait_result == -1 && errno == ECHILD;
	}

}  // namespace howdy::test::process
