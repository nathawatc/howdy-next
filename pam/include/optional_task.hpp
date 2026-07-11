#ifndef OPTIONAL_TASK_H_
#define OPTIONAL_TASK_H_

#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <thread>
#include <utility>

// A task executed only if activated.
template <typename T> class optional_task {
	std::thread             thread;
	std::packaged_task<T()> task;
	std::future<T>          future;
	bool                    spawned{false};
	bool                    is_active{false};

public:
	explicit optional_task(std::function<T()> func);
	void activate();
	template <typename R, typename P>
	auto               wait(std::chrono::duration<R, P> dur) -> std::future_status;
	[[nodiscard]] auto active() const -> bool;
	auto               ready() -> bool;
	auto               get() -> T;
	void               stop();
	~optional_task();
};

template <typename T>
optional_task<T>::optional_task(std::function<T()> func)
    : task(std::packaged_task<T()>(std::move(func)))
    , future(task.get_future()) {}

// Create a new thread and launch the task on it.
template <typename T> void optional_task<T>::activate() {
	thread    = std::thread(std::move(task));
	spawned   = true;
	is_active = true;
}

// Wait for `dur` time and return a `future` status.
template <typename T>
template <typename R, typename P>
auto optional_task<T>::wait(std::chrono::duration<R, P> dur) -> std::future_status {
	return future.wait_for(dur);
}

template <typename T> auto optional_task<T>::active() const -> bool {
	return is_active;
}

template <typename T> auto optional_task<T>::ready() -> bool {
	return spawned && future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

// Get the value.
// WARNING: The function should be run only if the task has successfully been
// stopped.
template <typename T> auto optional_task<T>::get() -> T {
	if (is_active || !spawned) {
		std::terminate();
	}

	return future.get();
}

// Stop the thread by joining it.
template <typename T> void optional_task<T>::stop() {
	if (!spawned) {
		return;
	}

	if (thread.joinable()) {
		thread.join();
	}

	is_active = false;
}

template <typename T> optional_task<T>::~optional_task() {
	if (is_active && spawned) {
		stop();
	}
}

#endif  // OPTIONAL_TASK_H_
