#include "runtime/auth_helper_process.hpp"
#include "runtime/runtime_session.hpp"
#include "runtime/runtime_session_process_test_support.hpp"
#include "runtime/runtime_session_test_groups.hpp"
#include "support/process_test_support.hpp"
#include "support/scoped_fd.hpp"
#include "test_support.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

// Child mode: a helper that ignores SIGTERM and never answers, so only the kill ends it. It
// creates `marker_path` once it is past setting that up.
auto RunStalledHelper(const char *marker_path) -> int {
	(void)signal(SIGTERM, SIG_IGN);
	const howdy::native::ScopedFd marker(
	    open(marker_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
	if (!marker.Valid()) {
		return EXIT_FAILURE;
	}
	for (;;) {
		pause();
	}
}

namespace {

	using howdy::test::Expect;
	using howdy::test::runtime_session::Logged;
	using howdy::test::runtime_session::SelfExecContext;
	using howdy::test::runtime_session::SelfExecOperations;

	// The deadline covers the whole exchange: a helper that never sends is stopped (it ignores
	// SIGTERM, so this takes the kill) and reaped.
	auto TestStalledHelper(const char *label, bool block_signals) -> bool {
		const auto      marker = howdy::test::runtime_session::MarkerPath("stalled");
		std::error_code error;
		std::filesystem::remove(marker, error);
		SelfExecContext context;
		context.mode = {"--stalled-helper", marker.string()};

		howdy::pam::RuntimeDescriptorSet descriptors;
		bool                             opened  = true;
		const auto                       started = std::chrono::steady_clock::now();
		{
			const howdy::test::process::ScopedSignalBlock blocked(block_signals ? SIGCHLD : 0);
			opened = howdy::pam::auth_helper_process::OpenRuntimeDescriptors(
			    "alice", &descriptors, SelfExecOperations(&context),
			    started + std::chrono::milliseconds(500));
		}
		const auto elapsed = std::chrono::steady_clock::now() - started;
		const bool marked  = std::filesystem::exists(marker, error);
		std::filesystem::remove(marker, error);
		return Expect(!opened && Logged(context, "timed out"),
		              std::string(label) + ": a silent helper times out") &&
		       Expect(marked, std::string(label) + ": the stalled helper was running") &&
		       Expect(elapsed < std::chrono::seconds(5),
		              std::string(label) + ": the deadline bounds the open") &&
		       Expect(context.spawned_pid > 0 &&
		                  howdy::test::process::ChildReaped(context.spawned_pid),
		              std::string(label) + ": the stalled helper is killed and reaped");
	}

}  // namespace

auto RunRuntimeSessionDeadlineTests() -> bool {
	bool ok = true;
	ok &= TestStalledHelper("stalled helper", false);
	ok &= TestStalledHelper("stalled helper with SIGCHLD blocked", true);
	return ok;
}
