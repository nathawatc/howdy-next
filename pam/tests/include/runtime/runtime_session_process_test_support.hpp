#pragma once

#include "runtime/auth_helper_process.hpp"
#include "runtime/child_process.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

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
		// The production spawn path, so the exchanges also cover pidfd_spawn.
		const int result = howdy::pam::runtime::SpawnChild(request.child_pid, request.child_pidfd,
		                                                   {
		                                                       .path    = "/proc/self/exe",
		                                                       .actions = request.actions,
		                                                       .argv    = argv.data(),
		                                                       .envp    = env.data(),
		                                                   });
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

	// A marker path unique to this process and `name`.
	inline auto MarkerPath(std::string_view name) -> std::filesystem::path {
		return std::filesystem::current_path() /
		       ("howdy-runtime-session-" + std::string(name) + "-" + std::to_string(getpid()));
	}

}  // namespace howdy::test::runtime_session
