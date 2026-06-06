#include "optional_task.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <string>

namespace {

    auto expect(bool condition, const std::string &message) -> bool {
        if (!condition) {
            std::cerr << "FAIL: " << message << "\n";
            return false;
        }
        return true;
    }

}  // namespace

auto main() -> int {
    bool ok = true;

    {
        optional_task<int> task([] {
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
        optional_task<int> task([gate_future] {
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
            optional_task<int> task([gate_future, &completed] {
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
