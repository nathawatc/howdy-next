#pragma once

#include "runtime/auth_helper_process.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <spawn.h>
#include <unistd.h>

#include <sys/types.h>

namespace howdy::test::runtime_session {

	// Runs this test binary in place of the auth helper, in one of its child modes (see main in
	// runtime_session_test.cpp), with the production file actions: /dev/null stdio, the socket at
	// fd 3, nothing else.
	struct SelfExecContext {
		std::vector<std::string> mode;
		std::vector<std::string> log_messages;
		std::vector<std::string> argv;
		std::vector<std::string> environment;
		pid_t                    spawned_pid = -1;
		int                      spawn_calls = 0;
	};

	inline void SelfExecLog(void *context, std::string_view message) {
		static_cast<SelfExecContext *>(context)->log_messages.emplace_back(message);
	}

	inline auto SelfExecSpawn(const howdy::pam::auth_helper_process::SpawnRequest &request) -> int {
		auto &self = *static_cast<SelfExecContext *>(request.context);
		++self.spawn_calls;
		for (const char *argument :
		     request.argv | std::views::take_while([](const char *value) -> bool {
			     return value != nullptr;
		     })) {
			self.argv.emplace_back(argument);
		}
		for (const char *entry :
		     request.envp | std::views::take_while([](const char *value) -> bool {
			     return value != nullptr;
		     })) {
			self.environment.emplace_back(entry);
		}
		std::vector<std::string> arguments{"pam_runtime_session_test"};
		arguments.insert(arguments.end(), self.mode.begin(), self.mode.end());
		std::vector<char *> argv;
		argv.reserve(arguments.size() + 1);
		for (auto &argument : arguments) {
			argv.push_back(argument.data());
		}
		argv.push_back(nullptr);
		std::array<char *, 1> env{nullptr};
		const int result = posix_spawn(request.child_pid, "/proc/self/exe", request.actions,
		                               nullptr, argv.data(), env.data());
		if (result == 0) {
			self.spawned_pid = *request.child_pid;
		}
		return result;
	}

	inline auto SelfExecOperations(SelfExecContext *context)
	    -> howdy::pam::auth_helper_process::Operations {
		auto operations         = howdy::pam::auth_helper_process::ProductionOperations();
		operations.context      = context;
		operations.spawn        = SelfExecSpawn;
		operations.log_observer = SelfExecLog;
		return operations;
	}

	inline auto Logged(const SelfExecContext &context, std::string_view text) -> bool {
		return std::ranges::any_of(context.log_messages, [&](const std::string &message) -> bool {
			return message.contains(text);
		});
	}

	// Waits up to `timeout` for a child mode to create `marker`.
	inline auto WaitForMarker(const std::filesystem::path &marker,
	                          std::chrono::milliseconds timeout = std::chrono::seconds(5)) -> bool {
		const auto      deadline = std::chrono::steady_clock::now() + timeout;
		std::error_code error;
		while (!std::filesystem::exists(marker, error)) {
			if (std::chrono::steady_clock::now() >= deadline) {
				return false;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return true;
	}

	// A marker path unique to this process and `name`.
	inline auto MarkerPath(std::string_view name) -> std::filesystem::path {
		return std::filesystem::current_path() /
		       ("howdy-runtime-session-" + std::string(name) + "-" + std::to_string(getpid()));
	}

}  // namespace howdy::test::runtime_session
