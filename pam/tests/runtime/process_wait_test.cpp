#include "protocol/compare_exit.hpp"
#include "protocol/compare_verdict.hpp"
#include "runtime/auth_helper_process/internal.hpp"
#include "runtime/child_process.hpp"
#include "runtime/compare_process.hpp"
#include "runtime/compare_process_test_support.hpp"
#include "support/eintr.hpp"
#include "support/process_status.hpp"
#include "support/process_test_support.hpp"
#include "support/scoped_fd.hpp"
#include "test_support.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>

#include <fcntl.h>
#include <signal.h>  // IWYU pragma: keep
#include <stdlib.h>
#include <unistd.h>

#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>

namespace {
	using howdy::test::Expect;
	using howdy::test::ScopedFd;
	using howdy::test::process::ChildReaped;
	using howdy::test::process::ScopedSigchldAction;
	using howdy::test::process::SpawnBlockedChild;
	using howdy::test::process::SpawnExitingChild;
	using howdy::test::process::SpawnSigtermIgnoringChild;

	auto AlwaysCancelCompare(void *context) -> bool {
		(void)context;
		return true;
	}

	struct TerminalBoundaryChild {
		pid_t child_pid              = -1;
		int   release_fd             = -1;
		bool  cancellation_requested = false;
		bool  child_exit_observed    = false;
	};

	auto SpawnTerminalBoundaryChild(int *release_fd) -> pid_t {
		std::array<int, 2> release_pipe = {-1, -1};
		if (pipe2(release_pipe.data(), O_CLOEXEC) != 0) {
			return -1;
		}

		const pid_t child_pid = fork();
		if (child_pid < 0) {
			(void)close(release_pipe.at(0));
			(void)close(release_pipe.at(1));
			return -1;
		}
		if (child_pid == 0) {
			(void)close(release_pipe.at(1));
			char          release     = '\0';
			const ssize_t read_result = howdy::native::RetryOnEintr([&] -> ssize_t {
				return read(release_pipe.at(0), &release, sizeof(release));
			});
			(void)close(release_pipe.at(0));
			_exit(read_result == 1 && release == '1' ? EXIT_SUCCESS : EXIT_FAILURE);
		}

		(void)close(release_pipe.at(0));
		*release_fd = release_pipe.at(1);
		return child_pid;
	}

	auto WaitForChildExit(pid_t child_pid) -> bool {
		siginfo_t info{};
		while (waitid(P_PID, static_cast<id_t>(child_pid), &info, WEXITED | WNOWAIT) < 0) {
			if (errno != EINTR) {
				return false;
			}
		}
		return info.si_pid == child_pid && info.si_code == CLD_EXITED &&
		       info.si_status == EXIT_SUCCESS;
	}

	auto ReleaseTerminalBoundaryChild(void *context) -> bool {
		auto         &boundary     = *static_cast<TerminalBoundaryChild *>(context);
		const char    release      = '1';
		const ssize_t write_result = howdy::native::RetryOnEintr([&] -> ssize_t {
			return write(boundary.release_fd, &release, sizeof(release));
		});
		if (write_result != 1) {
			return boundary.cancellation_requested;
		}

		boundary.child_exit_observed = WaitForChildExit(boundary.child_pid);
		return boundary.cancellation_requested;
	}

	auto ExpectTerminalOutcomePreserved(bool cancellation) -> bool {
		const std::string outcome = cancellation ? "cancellation" : "timeout";
		bool              ok      = true;

		int         release_pipe_fd = -1;
		const pid_t child_pid       = SpawnTerminalBoundaryChild(&release_pipe_fd);
		ScopedFd    release_fd(release_pipe_fd);
		ok &= Expect(child_pid > 0, outcome + " boundary spawns child");
		if (child_pid <= 0) {
			return false;
		}

		TerminalBoundaryChild boundary{
		    .child_pid              = child_pid,
		    .release_fd             = release_fd.Get(),
		    .cancellation_requested = cancellation,
		};
		const auto deadline = cancellation
		                          ? std::chrono::steady_clock::now() + std::chrono::seconds(1)
		                          : std::chrono::steady_clock::now();
		const int  status   = howdy::test::prompt_coordinator::WaitUntilForTestChild(
		    child_pid, deadline, &boundary, ReleaseTerminalBoundaryChild, -1);
		const auto expected_exit   = cancellation ? howdy::native::CompareExit::kAbort
		                                          : howdy::native::CompareExit::kTimeoutReached;
		const int  expected_status = howdy::native::EncodeWaitExitStatus(expected_exit);
		ok &= Expect(boundary.child_exit_observed,
		             outcome + " boundary observes child exit before cleanup wait");
		ok &= Expect(WIFEXITED(status) && status == expected_status,
		             outcome + " preserves terminal outcome after child exit");
		ok &= Expect(ChildReaped(child_pid), outcome + " boundary reaps child");
		return ok;
	}

	// A PAM application that reaps children itself can take the compare child first. The wait
	// must then fail closed without signalling: the PID no longer belongs to compare.
	auto ExpectReapedElsewhereFailsClosed() -> bool {
		bool        ok        = true;
		const pid_t child_pid = SpawnExitingChild(0);
		ok &= Expect(child_pid > 0, "spawns child that the application reaps");
		if (child_pid <= 0) {
			return false;
		}
		int status = 0;
		ok &= Expect(howdy::native::RetryOnEintr([&] -> pid_t {
			             return waitpid(child_pid, &status, 0);
		             }) == child_pid,
		             "application reaps the child first");
		const int result = howdy::test::prompt_coordinator::WaitUntilForTestChild(
		    child_pid, std::chrono::steady_clock::now() + std::chrono::seconds(1), nullptr, nullptr,
		    -1);
		const auto abort_code = static_cast<int>(howdy::native::CompareExit::kAbort);
		ok &= Expect(WIFEXITED(result) && WEXITSTATUS(result) == abort_code,
		             "wait on a child reaped elsewhere returns abort, not its success status");
		return ok;
	}

	// Starts a blocked grandchild and reaps its parent, leaving a live process that is not our
	// child. `alive_fd` reaches end of file once that process dies.
	auto SpawnUnrelatedProcess(howdy::native::ScopedFd &alive_fd) -> pid_t {
		std::array<int, 2> alive_pipe = {-1, -1};
		std::array<int, 2> pid_pipe   = {-1, -1};
		if (pipe2(alive_pipe.data(), O_CLOEXEC) != 0) {
			return -1;
		}
		howdy::native::ScopedFd alive_read(alive_pipe.at(0));
		howdy::native::ScopedFd alive_write(alive_pipe.at(1));
		if (pipe2(pid_pipe.data(), O_CLOEXEC) != 0) {
			return -1;
		}
		howdy::native::ScopedFd pid_read(pid_pipe.at(0));
		howdy::native::ScopedFd pid_write(pid_pipe.at(1));

		const pid_t parent = fork();
		if (parent == 0) {
			const pid_t grandchild = fork();
			if (grandchild == 0) {
				// Holds the write end of the alive pipe until it dies.
				while (true) {
					pause();
				}
			}
			const ssize_t written = write(pid_write.Get(), &grandchild, sizeof(grandchild));
			_exit(grandchild > 0 && std::cmp_equal(written, sizeof(grandchild)) ? EXIT_SUCCESS
			                                                                    : EXIT_FAILURE);
		}
		alive_write.Reset();
		pid_write.Reset();
		if (parent < 0) {
			return -1;
		}

		pid_t         grandchild = -1;
		const ssize_t read_size  = howdy::native::RetryOnEintr([&] -> ssize_t {
			return read(pid_read.Get(), &grandchild, sizeof(grandchild));
		});
		const bool    reaped     = howdy::test::process::ReapTestChild(parent, nullptr);
		if (!std::cmp_equal(read_size, sizeof(grandchild)) || grandchild <= 0) {
			return -1;
		}
		if (!reaped) {
			(void)kill(grandchild, SIGKILL);
			return -1;
		}
		alive_fd = std::move(alive_read);
		return grandchild;
	}

	// The helper paths share the same rule: a helper the application reaped is reported as reaped
	// elsewhere, never as success, and the terminate path reaps one that is still running.
	auto ExpectHelperTrackedThroughPidfd() -> bool {
		using howdy::pam::auth_helper_process::internal::HelperWaitResult;
		using howdy::pam::auth_helper_process::internal::TerminateAndReapHelperProcess;
		using howdy::pam::auth_helper_process::internal::WaitForHelperProcessUntil;
		bool ok = true;

		const pid_t reaped_child = SpawnExitingChild(0);
		ok &= Expect(reaped_child > 0, "spawns helper that the application reaps");
		if (reaped_child > 0) {
			int status = 0;
			ok &= Expect(howdy::native::RetryOnEintr([&] -> pid_t {
				             return waitpid(reaped_child, &status, 0);
			             }) == reaped_child,
			             "application reaps the helper first");
			TerminateAndReapHelperProcess(reaped_child, -1);
			int        wait_status = 0;
			const auto waited      = WaitForHelperProcessUntil(
			    reaped_child, std::chrono::steady_clock::now() + std::chrono::seconds(1),
			    &wait_status, -1);
			ok &= Expect(waited == HelperWaitResult::kReapedElsewhere,
			             "waiting on a helper reaped elsewhere reports that, not success");
		}

		// A live process that is not our child holds the PID, as after the application reaps
		// the helper and the PID is reused: terminating must leave it untouched.
		howdy::native::ScopedFd alive_fd;
		const pid_t             reused_pid = SpawnUnrelatedProcess(alive_fd);
		ok &= Expect(reused_pid > 0, "spawns live process that is not our child");
		if (reused_pid > 0) {
			TerminateAndReapHelperProcess(reused_pid, -1);
			pollfd alive = {.fd = alive_fd.Get(), .events = POLLIN, .revents = 0};
			ok &= Expect(poll(&alive, 1, 100) == 0,
			             "terminating a helper never signals a reused PID");
			(void)kill(reused_pid, SIGKILL);
		}

		const pid_t stuck_child = SpawnSigtermIgnoringChild();
		ok &= Expect(stuck_child > 0, "spawns SIGTERM-resistant helper");
		if (stuck_child > 0) {
			int        status = 0;
			const auto waited = WaitForHelperProcessUntil(
			    stuck_child, std::chrono::steady_clock::now(), &status, -1);
			ok &= Expect(waited == HelperWaitResult::kTimedOut, "helper deadline reports timeout");
			ok &= Expect(ChildReaped(stuck_child), "helper deadline kills and reaps the helper");
		}

		// The grace wait returns as soon as the helper exits, even with a stale EINTR in errno
		// from an earlier, retried call.
		const pid_t polite_child = SpawnBlockedChild();
		ok &= Expect(polite_child > 0, "spawns helper that exits on SIGTERM");
		if (polite_child > 0) {
			const howdy::native::ScopedFd pidfd(howdy::pam::runtime::OpenChildPidfd(polite_child));
			errno            = EINTR;
			const auto start = std::chrono::steady_clock::now();
			TerminateAndReapHelperProcess(polite_child, pidfd.Get());
			const auto elapsed = std::chrono::steady_clock::now() - start;
			ok &= Expect(ChildReaped(polite_child), "helper terminated by SIGTERM is reaped");
			ok &= Expect(elapsed < std::chrono::milliseconds(300),
			             "helper grace wait ends when the helper exits");
		}

		const pid_t exiting_child = SpawnExitingChild(5);
		ok &= Expect(exiting_child > 0, "spawns exiting helper");
		if (exiting_child > 0) {
			int        status = 0;
			const auto waited = WaitForHelperProcessUntil(
			    exiting_child, std::chrono::steady_clock::now() + std::chrono::seconds(5), &status,
			    -1);
			ok &= Expect(waited == HelperWaitResult::kExited && WIFEXITED(status) &&
			                 WEXITSTATUS(status) == 5,
			             "helper wait preserves exit status through the pidfd");
		}
		return ok;
	}

	auto ExpectSignalledChildStatus() -> bool {
		bool        ok        = true;
		const pid_t child_pid = SpawnBlockedChild();
		ok &= Expect(child_pid > 0, "spawns child to kill");
		if (child_pid <= 0) {
			return false;
		}
		ok &= Expect(kill(child_pid, SIGKILL) == 0, "kills compare child externally");
		const int status = howdy::test::prompt_coordinator::WaitUntilForTestChild(
		    child_pid, std::chrono::steady_clock::now() + std::chrono::seconds(5), nullptr, nullptr,
		    -1);
		ok &= Expect(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
		             "compare wait reports the terminating signal");
		ok &= Expect(ChildReaped(child_pid), "signalled compare child is reaped");
		return ok;
	}

	using SignalAction = struct sigaction;

	auto CurrentSigchldAction() -> SignalAction {
		SignalAction action{};
		(void)sigaction(SIGCHLD, nullptr, &action);
		return action;
	}

	// A compare stand-in: waits for the gate, writes `verdict` (unless negative) to its verdict
	// socket, then exits with `exit_code`. The gate lets the test take the pidfd before the child
	// can exit and, under SIG_IGN, be reaped by the kernel.
	struct VerdictChild {
		pid_t                   pid = -1;
		howdy::native::ScopedFd pidfd;
		howdy::native::ScopedFd verdict_read;
		howdy::native::ScopedFd gate_write;
	};

	// `reports` records of `record_bytes` bytes each, every byte `verdict`.
	// NOLINTBEGIN(bugprone-easily-swappable-parameters)
	auto StartVerdictChild(int verdict, int exit_code, int reports = 1,
	                       std::size_t record_bytes = 1) -> VerdictChild {
		// NOLINTEND(bugprone-easily-swappable-parameters)
		std::array<int, 2> gate{-1, -1};
		std::array<int, 2> verdict_socket{-1, -1};
		VerdictChild       child;
		if (pipe2(gate.data(), O_CLOEXEC) != 0) {
			return child;
		}
		if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0,
		               verdict_socket.data()) != 0) {
			(void)close(gate.at(0));
			(void)close(gate.at(1));
			return child;
		}
		child.pid = fork();
		if (child.pid == 0) {
			char go = '\0';
			if (read(gate.at(0), &go, 1) == 1 && verdict >= 0) {
				const std::array<unsigned char, 2> record{
				    static_cast<unsigned char>(verdict),
				    static_cast<unsigned char>(verdict),
				};
				for (int report = 0; report < reports; ++report) {
					if (write(verdict_socket.at(1), record.data(), record_bytes) !=
					    static_cast<ssize_t>(record_bytes)) {
						_exit(EXIT_FAILURE);
					}
				}
			}
			_exit(exit_code);
		}
		(void)close(gate.at(0));
		(void)close(verdict_socket.at(1));
		child.gate_write.Reset(gate.at(1));
		child.verdict_read.Reset(verdict_socket.at(0));
		if (child.pid > 0) {
			child.pidfd.Reset(howdy::pam::runtime::OpenChildPidfd(child.pid));
		}
		return child;
	}

	auto RunVerdictChild(VerdictChild &child) -> int {
		const char go = '1';
		if (write(child.gate_write.Get(), &go, 1) != 1) {
			return -1;
		}
		return howdy::pam::compare_process::WaitUntil(
		    child.pid, child.pidfd.Get(), child.verdict_read.Get(),
		    std::chrono::steady_clock::now() + std::chrono::seconds(2), nullptr, nullptr, -1);
	}

	// With SIGCHLD ignored the kernel reaps compare on exit and its exit status is gone; the
	// verdict it wrote stands in, with no change to the application's SIGCHLD action.
	auto ExpectVerdictWhenReapedElsewhere() -> bool {
		using howdy::native::CompareExit;
		const auto encode = [](CompareExit exit) -> int {
			return howdy::native::EncodeWaitExitStatus(exit);
		};
		bool ok = true;
		{
			const ScopedSigchldAction ignored(SIG_IGN, 0);
			ok &= Expect(ignored.Valid(), "verdict test ignores SIGCHLD");
			for (const auto verdict : {CompareExit::kSuccess, CompareExit::kTooDark}) {
				auto child =
				    StartVerdictChild(static_cast<int>(verdict), static_cast<int>(verdict));
				ok &= Expect(child.pidfd.Valid(), "verdict child has a pidfd");
				ok &= Expect(RunVerdictChild(child) == encode(verdict),
				             "compare reaped by the kernel reports through its verdict");
			}
			auto silent = StartVerdictChild(-1, static_cast<int>(CompareExit::kSuccess));
			ok &= Expect(silent.pidfd.Valid(), "silent child has a pidfd");
			ok &= Expect(RunVerdictChild(silent) == encode(CompareExit::kAbort),
			             "compare reaped without a verdict fails closed");
			auto twice = StartVerdictChild(static_cast<int>(CompareExit::kSuccess),
			                               static_cast<int>(CompareExit::kSuccess), 2);
			ok &= Expect(twice.pidfd.Valid(), "double-report child has a pidfd");
			ok &= Expect(RunVerdictChild(twice) == encode(CompareExit::kAbort),
			             "more than one reported result fails closed");
			auto longer = StartVerdictChild(static_cast<int>(CompareExit::kSuccess),
			                                static_cast<int>(CompareExit::kSuccess), 1, 2);
			ok &= Expect(longer.pidfd.Valid(), "long-record child has a pidfd");
			ok &= Expect(RunVerdictChild(longer) == encode(CompareExit::kAbort),
			             "a result record longer than one byte fails closed");
			// A timeout stays a timeout when the kernel reaps compare after the wait's SIGTERM.
			const pid_t                   blocked = SpawnBlockedChild();
			const howdy::native::ScopedFd blocked_pidfd(
			    howdy::pam::runtime::OpenChildPidfd(blocked));
			ok &= Expect(blocked > 0 && blocked_pidfd.Valid(), "blocked child has a pidfd");
			const int timed_out = howdy::pam::compare_process::WaitUntil(
			    blocked, blocked_pidfd.Get(), -1,
			    std::chrono::steady_clock::now() + std::chrono::milliseconds(40), nullptr, nullptr,
			    -1);
			ok &= Expect(timed_out == encode(CompareExit::kTimeoutReached),
			             "a timeout survives the kernel reaping compare");
			ok &= Expect(CurrentSigchldAction().sa_handler == SIG_IGN,
			             "the compare wait leaves the application's SIGCHLD action alone");
		}
		auto reaped = StartVerdictChild(static_cast<int>(CompareExit::kSuccess),
		                                static_cast<int>(CompareExit::kTooDark));
		ok &= Expect(reaped.pidfd.Valid(), "exit-status child has a pidfd");
		ok &= Expect(RunVerdictChild(reaped) == encode(CompareExit::kTooDark) &&
		                 ChildReaped(reaped.pid),
		             "an available exit status decides over the verdict");
		return ok;
	}

	// Spawn hands back PAM's end of a verdict socket whose other end only the child holds.
	auto SpawnVerdictWriter(const howdy::pam::compare_process::SpawnRequest &request) -> int {
		auto &capture =
		    *static_cast<howdy::test::prompt_coordinator::PosixSpawnCapture *>(request.context);
		const pid_t child_pid = fork();
		if (child_pid < 0) {
			return errno;
		}
		if (child_pid == 0) {
			char go = '\0';
			// Spawn takes the pidfd before the gate opens, so the kernel cannot reap the child
			// first.
			if (read(capture.spawn_gate_fd, &go, 1) != 1) {
				_exit(EXIT_FAILURE);
			}
			const auto byte    = static_cast<unsigned char>(howdy::native::CompareExit::kTooDark);
			const auto verdict = howdy::test::prompt_coordinator::Dup2SourceFor(
			    capture, howdy::native::compare_verdict::kFd);
			_exit(verdict.has_value() && write(*verdict, &byte, 1) == 1 ? EXIT_SUCCESS
			                                                            : EXIT_FAILURE);
		}
		capture.next_pid   = child_pid;
		*request.child_pid = child_pid;
		return 0;
	}

	auto ExpectSpawnWiresVerdictSocket() -> bool {
		std::array<int, 2> gate{-1, -1};
		if (!Expect(pipe2(gate.data(), O_CLOEXEC) == 0, "spawn verdict test opens its gate")) {
			return false;
		}
		const howdy::native::ScopedFd                      gate_read(gate.at(0));
		const howdy::native::ScopedFd                      gate_write(gate.at(1));
		const ScopedSigchldAction                          ignored(SIG_IGN, 0);
		howdy::test::prompt_coordinator::PosixSpawnCapture capture;
		capture.spawn_gate_fd = gate_read.Get();
		auto operations       = howdy::test::prompt_coordinator::PosixSpawnOperations(&capture);
		operations.spawn      = SpawnVerdictWriter;
		pid_t                                                      child_pid      = -1;
		int                                                        raw_pidfd      = -1;
		int                                                        raw_verdict_fd = -1;
		const howdy::test::prompt_coordinator::CompareRequestFiles files;
		const int result = howdy::pam::compare_process::Spawn(
		    files.Request(), &child_pid, &raw_pidfd, &raw_verdict_fd, operations);
		const howdy::native::ScopedFd pidfd(raw_pidfd);
		const howdy::native::ScopedFd verdict_fd(raw_verdict_fd);
		bool ok = Expect(ignored.Valid() && result == 0 && pidfd.Valid() && verdict_fd.Valid(),
		                 "spawn returns a pidfd and a verdict socket");
		const char go = '1';
		ok &= Expect(write(gate_write.Get(), &go, 1) == 1, "spawn verdict test opens the gate");
		const int status = howdy::pam::compare_process::WaitUntil(
		    child_pid, pidfd.Get(), verdict_fd.Get(),
		    std::chrono::steady_clock::now() + std::chrono::seconds(2), nullptr, nullptr, -1);
		ok &= Expect(status ==
		                 howdy::native::EncodeWaitExitStatus(howdy::native::CompareExit::kTooDark),
		             "the spawned child's verdict reaches the wait under SIG_IGN");
		return ok;
	}

	auto ExpectDeadlinePollTimeoutBounds() -> bool {
		using howdy::pam::auth_helper_process::internal::DeadlinePollTimeout;
		using howdy::pam::auth_helper_process::internal::HelperDeadline;
		const auto now         = std::chrono::steady_clock::now();
		const int  bounded_pos = DeadlinePollTimeout(now + std::chrono::milliseconds(50));
		return Expect(DeadlinePollTimeout(HelperDeadline::min()) == 0,
		              "minimum deadline clamps poll timeout to zero") &&
		       Expect(DeadlinePollTimeout(now - std::chrono::milliseconds(1)) == 0,
		              "past deadline clamps poll timeout to zero") &&
		       Expect(DeadlinePollTimeout(HelperDeadline::max()) == std::numeric_limits<int>::max(),
		              "distant future deadline saturates poll timeout at INT_MAX") &&
		       Expect(bounded_pos > 0 && bounded_pos <= 100,
		              "near-term deadline stays positive within millisecond range");
	}

	auto ExpectProcessWaiting() -> bool {
		bool ok = true;
		ok &= ExpectDeadlinePollTimeoutBounds();
		const pid_t compare_child_pid = SpawnExitingChild(7);
		ok &= Expect(compare_child_pid > 0, "spawns compare child");
		if (compare_child_pid > 0) {
			const int status = howdy::test::prompt_coordinator::WaitUntilForTestChild(
			    compare_child_pid, std::chrono::steady_clock::now() + std::chrono::seconds(1),
			    nullptr, nullptr, -1);
			ok &= Expect(WIFEXITED(status) && WEXITSTATUS(status) == 7,
			             "compare wait preserves child exit status");
		}

		const pid_t helper_child_pid = SpawnExitingChild(9);
		ok &= Expect(helper_child_pid > 0, "spawns helper child");
		if (helper_child_pid > 0) {
			int        status = 0;
			const auto waited =
			    howdy::pam::auth_helper_process::internal::WaitForHelperProcessUntil(
			        helper_child_pid, std::chrono::steady_clock::now() + std::chrono::seconds(5),
			        &status, -1);
			ok &= Expect(
			    waited == howdy::pam::auth_helper_process::internal::HelperWaitResult::kExited &&
			        WIFEXITED(status) && WEXITSTATUS(status) == 9,
			    "helper wait preserves child exit status");
		}

		constexpr pid_t nonexistent_child = std::numeric_limits<pid_t>::max();
		const int       compare_failure   = howdy::test::prompt_coordinator::WaitUntilForTestChild(
		    nonexistent_child, std::chrono::steady_clock::now() + std::chrono::seconds(1), nullptr,
		    nullptr, -1);
		const auto abort_code = static_cast<int>(howdy::native::CompareExit::kAbort);
		ok &= Expect(WIFEXITED(compare_failure) && WEXITSTATUS(compare_failure) == abort_code,
		             "compare wait failure returns abort status");
		int helper_failure_status = 0;
		ok &= Expect(
		    howdy::pam::auth_helper_process::internal::WaitForHelperProcessUntil(
		        nonexistent_child, std::chrono::steady_clock::now() + std::chrono::seconds(1),
		        &helper_failure_status, -1) ==
		        howdy::pam::auth_helper_process::internal::HelperWaitResult::kReapedElsewhere,
		    "helper wait on a process that is not a child reports it gone");
		// Past the deadline, a helper gone elsewhere counts as a timeout, as one the wait
		// collects late does; the prepare uses the same rule for a helper gone before its pidfd.
		ok &= Expect(howdy::pam::auth_helper_process::internal::WaitForHelperProcessUntil(
		                 nonexistent_child,
		                 std::chrono::steady_clock::now() - std::chrono::milliseconds(1),
		                 &helper_failure_status, -1) ==
		                 howdy::pam::auth_helper_process::internal::HelperWaitResult::kTimedOut,
		             "helper gone elsewhere after its deadline counts as a timeout");

		// Any other wait failure is an error, not a helper gone elsewhere: a descriptor that is
		// not a pidfd makes the pidfd wait fail with EBADF while the child still runs.
		const pid_t unwaitable_child = SpawnBlockedChild();
		const int   not_a_pidfd      = open("/dev/null", O_RDONLY | O_CLOEXEC);
		ok &=
		    Expect(unwaitable_child > 0 && not_a_pidfd >= 0, "spawns helper child to fail waiting");
		if (unwaitable_child > 0 && not_a_pidfd >= 0) {
			int        unwaitable_status = 0;
			const auto waited =
			    howdy::pam::auth_helper_process::internal::WaitForHelperProcessUntil(
			        unwaitable_child, std::chrono::steady_clock::now() + std::chrono::seconds(1),
			        &unwaitable_status, not_a_pidfd);
			ok &= Expect(
			    waited == howdy::pam::auth_helper_process::internal::HelperWaitResult::kWaitError,
			    "helper wait failure is reported as an error");
		}
		if (not_a_pidfd >= 0) {
			(void)close(not_a_pidfd);
		}
		if (unwaitable_child > 0) {
			// The failed wait could not stop it either, so the test does.
			(void)kill(unwaitable_child, SIGKILL);
			ok &= Expect(howdy::native::RetryOnEintr([&] -> pid_t {
				             return waitpid(unwaitable_child, nullptr, 0);
			             }) == unwaitable_child,
			             "reaps helper child after failed wait");
		}

		const pid_t cancelled_child = SpawnBlockedChild();
		ok &= Expect(cancelled_child > 0, "spawns cancellable compare child");
		if (cancelled_child > 0) {
			const int cancelled_status = howdy::test::prompt_coordinator::WaitUntilForTestChild(
			    cancelled_child, std::chrono::steady_clock::now() + std::chrono::seconds(1),
			    nullptr, AlwaysCancelCompare, -1);
			ok &= Expect(WIFEXITED(cancelled_status) && WEXITSTATUS(cancelled_status) == abort_code,
			             "compare cancellation returns abort status");
			ok &= Expect(ChildReaped(cancelled_child), "compare cancellation reaps child");
		}

		ok &= ExpectTerminalOutcomePreserved(false);
		ok &= ExpectTerminalOutcomePreserved(true);
		ok &= ExpectReapedElsewhereFailsClosed();
		ok &= ExpectSignalledChildStatus();
		ok &= ExpectHelperTrackedThroughPidfd();
		ok &= ExpectVerdictWhenReapedElsewhere();
		ok &= ExpectSpawnWiresVerdictSocket();

		const pid_t cleanup_child = SpawnSigtermIgnoringChild();
		ok &= Expect(cleanup_child > 0, "spawns compare child for noexcept cleanup");
		if (cleanup_child > 0) {
			howdy::pam::compare_process::CancelAndReap(cleanup_child, -1);
			ok &= Expect(ChildReaped(cleanup_child), "noexcept compare cleanup reaps child");
		}

		const pid_t timed_out_child = SpawnSigtermIgnoringChild();
		ok &= Expect(timed_out_child > 0, "spawns SIGTERM-resistant compare child");
		if (timed_out_child > 0) {
			const int timed_out_status = howdy::test::prompt_coordinator::WaitUntilForTestChild(
			    timed_out_child, std::chrono::steady_clock::now(), nullptr, nullptr, -1);
			const int timeout_code = static_cast<int>(howdy::native::CompareExit::kTimeoutReached);
			ok &=
			    Expect(WIFEXITED(timed_out_status) && WEXITSTATUS(timed_out_status) == timeout_code,
				       "compare timeout kills SIGTERM-resistant child");
			ok &= Expect(ChildReaped(timed_out_child),
			             "compare timeout reaps SIGTERM-resistant child");
		}

		return ok;
	}

}  // namespace

auto main() -> int {
	return ExpectProcessWaiting() ? 0 : 1;
}
