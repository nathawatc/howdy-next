#include "prompt/optional_task.hpp"
#include "test_support.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <future>
#include <string>
#include <string_view>
#include <unistd.h>

#include <sys/types.h>
#include <sys/wait.h>

namespace {

	using howdy::test::expect;

	auto run_get_before_spawn() -> void {
		optional_task<int> task([] -> int {
			return 1;
		});
		task.get();
	}

	auto run_get_while_active() -> void {
		std::array<int, 2> ready_pipe{};
		if (pipe(ready_pipe.data()) != 0) {
			_exit(1);
		}

		std::promise<void> gate;
		const auto         gate_future = gate.get_future().share();
		optional_task<int> task([gate_future, ready_fd = ready_pipe[1]] -> int {
			const char ready = 'r';
			if (write(ready_fd, &ready, sizeof(ready)) != sizeof(ready)) {
				_exit(1);
			}
			close(ready_fd);
			gate_future.wait();
			return 1;
		});

		task.activate();
		char ready = 0;
		if (read(ready_pipe[0], &ready, sizeof(ready)) != sizeof(ready)) {
			gate.set_value();
			task.stop();
			close(ready_pipe[0]);
			_exit(1);
		}
		close(ready_pipe[0]);
		task.get();
	}

	auto expect_terminates(void (*child_fn)(), const std::string &message) -> bool {
		const pid_t child = fork();
		if (child == -1) {
			return expect(false, message + ": fork failed");
		}
		if (child == 0) {
			child_fn();
			_exit(1);
		}

		int status = 0;
		if (waitpid(child, &status, 0) == -1) {
			return expect(false, message + ": waitpid failed");
		}
		return expect(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT, message);
	}

}  // namespace

auto main(int argc, char **argv) -> int {
	if (argc == 2 && std::string_view(argv[1]) == "get-before-spawn") {
		return expect_terminates(run_get_before_spawn, "get before spawn terminates") ? 0 : 1;
	}
	if (argc == 2 && std::string_view(argv[1]) == "get-while-active") {
		return expect_terminates(run_get_while_active, "get while active terminates") ? 0 : 1;
	}

	bool ok = true;

	{
		optional_task<int> task([] -> int {
			return 1;
		});

		ok &= expect(!task.active(), "inactive task reports inactive");
		ok &= expect(!task.ready(), "inactive task reports not ready");
		task.stop();
		ok &= expect(!task.active(), "stopping inactive task is harmless");
	}

	{
		std::promise<void> gate;
		const auto         gate_future = gate.get_future().share();
		optional_task<int> task([gate_future] -> int {
			gate_future.wait();
			return 42;
		});

		task.activate();
		ok &= expect(task.active(), "activated task reports active");
		ok &= expect(!task.ready(), "blocked active task reports not ready");
		ok &= expect(task.wait(std::chrono::milliseconds(0)) == std::future_status::timeout,
		             "blocked active task wait reports timeout");

		gate.set_value();
		ok &= expect(task.wait(std::chrono::seconds(1)) == std::future_status::ready,
		             "released task becomes ready");
		ok &= expect(task.ready(), "released task reports ready");
		ok &= expect(task.active(), "ready task remains active until stopped");

		task.stop();
		ok &= expect(!task.active(), "stopped task reports inactive");
		task.stop();
		ok &= expect(!task.active(), "repeated stop is harmless");
		ok &= expect(task.get() == 42, "stopped task returns its value");
	}

	{
		std::promise<void> gate;
		const auto         gate_future = gate.get_future().share();
		std::atomic<bool>  completed{false};

		{
			optional_task<int> task([gate_future, &completed] -> int {
				gate_future.wait();
				completed.store(true);
				return 0;
			});
			task.activate();
			gate.set_value();
		}

		ok &= expect(completed.load(), "destructor joins an active task");
	}

	return ok ? 0 : 1;
}
