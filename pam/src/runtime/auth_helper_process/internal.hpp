#pragma once

#include "runtime/auth_helper_process.hpp"
#include "support/scoped_fd.hpp"

#include <array>
#include <span>
#include <string_view>

#include <spawn.h>

#include <sys/types.h>

namespace howdy::pam::auth_helper_process::internal {

	class __attribute__((visibility("hidden"))) ProcessChannel {
	public:
		[[nodiscard]] auto Data() noexcept -> int * {
			return descriptors_.data();
		}

		[[nodiscard]] auto Descriptors() noexcept -> std::span<int> {
			return descriptors_;
		}

		[[nodiscard]] auto Parent() noexcept -> int & {
			return descriptors_.front();
		}

		[[nodiscard]] auto Child() noexcept -> int & {
			return descriptors_.back();
		}

	private:
		std::array<int, 2> descriptors_ = {-1, -1};
	};

	struct __attribute__((visibility("hidden"))) HelperSpawn {
		// The helper's descriptor socket: PAM keeps Parent(), the helper gets Child() at fd 3.
		ProcessChannel             socket;
		posix_spawn_file_actions_t actions{};
		bool                       actions_initialized = false;
		// -1 once the helper was reaped, or when it was already gone as its pidfd was taken.
		pid_t child_pid = -1;
		// Taken right after spawn, so later waits and signals name the helper itself even if
		// the PAM application reaps it and the PID is reused.
		howdy::native::ScopedFd child_pidfd;
	};

	// Closes what the spawn still holds and stops and reaps a helper that is still ours.
	class __attribute__((visibility("hidden"))) HelperSpawnCleanup {
	public:
		HelperSpawnCleanup(const Operations &operations, HelperSpawn &spawn);
		~HelperSpawnCleanup();
		HelperSpawnCleanup(const HelperSpawnCleanup &)                     = delete;
		auto operator=(const HelperSpawnCleanup &) -> HelperSpawnCleanup & = delete;
		HelperSpawnCleanup(HelperSpawnCleanup &&)                          = delete;
		auto operator=(HelperSpawnCleanup &&) -> HelperSpawnCleanup &      = delete;

	private:
		const Operations &operations_;
		HelperSpawn      &spawn_;
	};

	// spawn.cpp
	// Logs `message` to syslog at `priority` and to the log observer.
	__attribute__((visibility("hidden"))) void
	LogAuthHelperMessage(const Operations &operations, int priority, std::string_view message);
	// Logs "<operation> failed for auth helper: <error text> (<error_code>)" to syslog and the
	// log observer, falling back to a plain syslog line if building the message fails.
	__attribute__((visibility("hidden"))) void LogAuthHelperFailure(const Operations &operations,
	                                                                const char       *operation,
	                                                                int error_code) noexcept;
	__attribute__((visibility("hidden"))) auto ProductionOperations() -> Operations;
	__attribute__((visibility("hidden"))) void CloseOwnedFd(const Operations &operations,
	                                                        int              &fd) noexcept;
	__attribute__((visibility("hidden"))) auto SetupHelperSpawn(const Operations &operations,
	                                                            HelperSpawn      *spawn) -> bool;
	__attribute__((visibility("hidden"))) auto SpawnOpenHelper(std::string_view  username,
	                                                           const Operations &operations,
	                                                           HelperSpawn      *spawn) -> bool;

}  // namespace howdy::pam::auth_helper_process::internal
