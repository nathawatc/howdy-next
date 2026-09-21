#include "../../src/runtime/auth_helper_process/internal.hpp"
#include "paths.hpp"
#include "protocol/auth_helper_protocol.hpp"
#include "runtime/auth_helper_process.hpp"
#include "runtime/runtime_session.hpp"
#include "runtime/runtime_session_process_test_support.hpp"
#include "runtime/runtime_session_test_groups.hpp"
#include "support/bit_flags.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/file.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>  // IWYU pragma: keep
#include <sys/wait.h>

namespace {
	using howdy::test::Expect;
	using howdy::test::runtime_session::AuthHelperSpawnFake;

	void ThrowingAuthHelperSpawnLog(void *context, std::string_view message) {
		(void)context;
		(void)message;
		throw std::runtime_error("injected log observer failure");
	}

	auto TestAuthHelperSpawnSetup() -> bool {
		struct FailureCase {
			std::string      operation;
			int              expected_destroy_calls;
			std::vector<int> expected_closed_fds;
		};

		const std::vector<FailureCase> failures = {
		    {.operation = "pipe2", .expected_destroy_calls = 0, .expected_closed_fds = {}},
		    {.operation              = "socketpair",
		     .expected_destroy_calls = 0,
		     .expected_closed_fds    = {10, 11}},
		    {.operation              = "actions_init",
		     .expected_destroy_calls = 0,
		     .expected_closed_fds    = {10, 11, 12, 13}},
		    {.operation              = "close_output_read",
		     .expected_destroy_calls = 1,
		     .expected_closed_fds    = {10, 11, 12, 13}},
		    {.operation              = "dup_stdout",
		     .expected_destroy_calls = 1,
		     .expected_closed_fds    = {10, 11, 12, 13}},
		    {.operation              = "dup_stderr",
		     .expected_destroy_calls = 1,
		     .expected_closed_fds    = {10, 11, 12, 13}},
		    {.operation              = "close_output_write",
		     .expected_destroy_calls = 1,
		     .expected_closed_fds    = {10, 11, 12, 13}},
		    {.operation              = "close_lease_parent",
		     .expected_destroy_calls = 1,
		     .expected_closed_fds    = {10, 11, 12, 13}},
		    {.operation              = "dup_lease",
		     .expected_destroy_calls = 1,
		     .expected_closed_fds    = {10, 11, 12, 13}},
		    {.operation              = "close_lease_child",
		     .expected_destroy_calls = 1,
		     .expected_closed_fds    = {10, 11, 12, 13}},
		    {.operation              = "closefrom",
		     .expected_destroy_calls = 1,
		     .expected_closed_fds    = {10, 11, 12, 13}},
		};
		const std::vector<std::string> setup_order = {
		    "pipe2",      "socketpair",        "actions_init",       "close_output_read",
		    "dup_stdout", "dup_stderr",        "close_output_write", "close_lease_parent",
		    "dup_lease",  "close_lease_child", "closefrom",
		};
		bool ok = true;
		for (const auto &failure : failures) {
			AuthHelperSpawnFake              fake{.fail_operation = failure.operation};
			howdy::pam::PreparedRuntimeFiles prepared;
			const bool result = howdy::pam::auth_helper_process::PrepareRuntimeAuthFiles(
			    "alice", &prepared, SpawnOperations(&fake));
			ok &= Expect(!result, failure.operation + " setup failure is rejected");
			ok &=
			    Expect(fake.spawn_calls == 0, failure.operation + " setup failure does not spawn");
			ok &= Expect(fake.destroy_calls == failure.expected_destroy_calls,
			             failure.operation + " has exact action destroy count");
			ok &= ExpectClosedExactlyOnce(fake, failure.expected_closed_fds, failure.operation);
			const auto failed = std::ranges::find(setup_order, failure.operation);
			ok &= ExpectOperationPrefix(
			    fake,
			    std::vector<std::string>(setup_order.begin(), failed == setup_order.end()
			                                                      ? setup_order.end()
			                                                      : failed + 1),
			    failure.operation);
		}

		AuthHelperSpawnFake              setup_destroy_failure{.fail_operation = "dup_stdout",
		                                                       .destroy_fails  = true};
		howdy::pam::PreparedRuntimeFiles setup_ignored;
		ok &= Expect(!howdy::pam::auth_helper_process::PrepareRuntimeAuthFiles(
		                 "alice", &setup_ignored, SpawnOperations(&setup_destroy_failure)),
		             "combined setup and destroy failure is rejected");
		ok &= ExpectLog(setup_destroy_failure, 0, "posix_spawn_file_actions_adddup2(STDOUT_FILENO)",
		                "combined setup and destroy failure");
		ok &= ExpectLog(setup_destroy_failure, 1, "posix_spawn_file_actions_destroy",
		                "combined setup and destroy failure");

		AuthHelperSpawnFake throwing_socket_log{.fail_operation = "socketpair"};
		auto                throwing_socket_operations = SpawnOperations(&throwing_socket_log);
		throwing_socket_operations.log_observer        = ThrowingAuthHelperSpawnLog;
		howdy::pam::PreparedRuntimeFiles throwing_socket_ignored;
		ok &= Expect(!howdy::pam::auth_helper_process::PrepareRuntimeAuthFiles(
		                 "alice", &throwing_socket_ignored, throwing_socket_operations),
		             "throwing socketpair failure logger is contained");
		ok &= ExpectClosedExactlyOnce(throwing_socket_log, {10, 11},
		                              "throwing socketpair failure logger");

		AuthHelperSpawnFake throwing_setup_log{.fail_operation = "dup_stdout"};
		auto                throwing_setup_operations = SpawnOperations(&throwing_setup_log);
		throwing_setup_operations.log_observer        = ThrowingAuthHelperSpawnLog;
		howdy::pam::PreparedRuntimeFiles throwing_setup_ignored;
		ok &= Expect(!howdy::pam::auth_helper_process::PrepareRuntimeAuthFiles(
		                 "alice", &throwing_setup_ignored, throwing_setup_operations),
		             "throwing setup failure logger is contained");
		ok &= Expect(throwing_setup_log.destroy_calls == 1,
		             "throwing setup failure logger still destroys actions");
		ok &= ExpectClosedExactlyOnce(throwing_setup_log, {10, 11, 12, 13},
		                              "throwing setup failure logger");

		AuthHelperSpawnFake              fake{.fail_operation = "spawn"};
		howdy::pam::PreparedRuntimeFiles prepared;
		const bool result = howdy::pam::auth_helper_process::PrepareRuntimeAuthFiles(
		    "alice", &prepared, SpawnOperations(&fake));
		ok &= Expect(!result, "spawn failure is rejected");
		ok &= ExpectLog(fake, 0, "posix_spawn", "spawn failure");
		ok &= Expect(fake.spawn_calls == 1, "spawn failure attempts spawn once");
		ok &= Expect(fake.destroy_calls == 1, "spawn failure destroys actions once");
		ok &= ExpectClosedExactlyOnce(fake, {10, 11, 12, 13}, "spawn failure");
		ok &=
		    Expect(fake.pipe_flags == std::vector<int>{O_CLOEXEC}, "output pipe is close-on-exec");
		ok &=
		    Expect(fake.socket_parameters ==
		               std::vector<std::array<int, 3>>{{AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0}},
		           "lease socketpair is sequenced-packet and close-on-exec");
		ok &= Expect(fake.dup2_fds == std::vector<std::pair<int, int>>{{11, STDOUT_FILENO},
		                                                               {11, STDERR_FILENO},
		                                                               {13, 3}},
		             "child receives output and lease fd 3");
		ok &= Expect(fake.action_closefrom_fds == std::vector<int>{4},
		             "child closes descriptors from fd 4");
		ok &= Expect(fake.action_close_fds == std::vector<int>{10, 11, 12, 13},
		             "child closes original pipe and socket descriptors");
		ok &= Expect(fake.spawn_argv ==
		                     std::vector<std::string>{kAuthHelperPath, "prepare", "alice"} &&
		                 fake.spawn_env.empty(),
		             "prepare uses exact argv and empty environment");
		ok &= Expect(fake.operations ==
		                 std::vector<std::string>{
		                     "pipe2", "socketpair", "actions_init", "close_output_read",
		                     "dup_stdout", "dup_stderr", "close_output_write", "close_lease_parent",
		                     "dup_lease", "close_lease_child", "closefrom", "spawn", "destroy",
		                     "close", "close", "close", "close"},
		             "prepare owns actions and descriptors in order");

		AuthHelperSpawnFake spawn_destroy_failure{.fail_operation = "spawn", .destroy_fails = true};
		howdy::pam::PreparedRuntimeFiles spawn_ignored;
		ok &= Expect(!howdy::pam::auth_helper_process::PrepareRuntimeAuthFiles(
		                 "alice", &spawn_ignored, SpawnOperations(&spawn_destroy_failure)),
		             "combined spawn and destroy failure is rejected");
		ok &= ExpectLog(spawn_destroy_failure, 0, "posix_spawn",
		                "combined spawn and destroy failure");
		ok &= ExpectLog(spawn_destroy_failure, 1, "posix_spawn_file_actions_destroy",
		                "combined spawn and destroy failure");

		AuthHelperSpawnFake              destroy_failure{.destroy_fails = true};
		howdy::pam::PreparedRuntimeFiles ignored;
		ok &= Expect(!howdy::pam::auth_helper_process::PrepareRuntimeAuthFiles(
		                 "alice", &ignored, SpawnOperations(&destroy_failure),
		                 std::chrono::steady_clock::now() + std::chrono::seconds(1)),
		             "child EOF still rejects prepare after destroy failure");
		ok &= Expect(destroy_failure.destroy_calls == 1 && destroy_failure.spawned_pid > 0,
		             "post-spawn destroy failure preserves child lifecycle");
		ok &= ExpectLog(destroy_failure, 0, "posix_spawn_file_actions_destroy", "destroy failure");
		errno = 0;
		ok &= Expect(waitpid(destroy_failure.spawned_pid, nullptr, WNOHANG) < 0 && errno == ECHILD,
		             "destroy failure child is reaped");
		return ok;
	}

	auto TestDescriptorCollisions() -> bool {
		AuthHelperSpawnFake              duplicate_failure{.fail_operation = "duplicate_fd",
		                                                   .next_pipe_fds  = {STDOUT_FILENO, 11}};
		howdy::pam::PreparedRuntimeFiles failed_prepare;
		bool ok = Expect(!howdy::pam::auth_helper_process::PrepareRuntimeAuthFiles(
		                     "alice", &failed_prepare, SpawnOperations(&duplicate_failure)),
		                 "descriptor normalization failure is rejected");
		ok &= ExpectClosedExactlyOnce(duplicate_failure, {STDOUT_FILENO, 11},
		                              "descriptor normalization failure");
		AuthHelperSpawnFake lease_collision{.fail_operation = "spawn", .next_socket_fds = {3, 12}};
		howdy::pam::PreparedRuntimeFiles lease_prepare;
		ok &= Expect(!howdy::pam::auth_helper_process::PrepareRuntimeAuthFiles(
		                 "alice", &lease_prepare, SpawnOperations(&lease_collision)),
		             "lease fd 3 collision reaches spawn");
		ok &= Expect(lease_collision.duplicate_fd_requests ==
		                 std::vector<std::pair<int, int>>{{3, 4}},
		             "lease fd 3 is normalized above reserved target");
		ok &= Expect(lease_collision.dup2_fds ==
		                 std::vector<std::pair<int, int>>{{11, 1}, {11, 2}, {12, 3}},
		             "normalized lease parent cannot close child fd 3");
		ok &= ExpectClosedExactlyOnce(lease_collision, {3, 20, 10, 11, 12}, "lease fd 3 collision");

		for (const auto fds :
		     {std::array{STDOUT_FILENO, STDERR_FILENO}, std::array{STDERR_FILENO, STDOUT_FILENO}}) {
			AuthHelperSpawnFake              fake{.fail_operation = "spawn", .next_pipe_fds = fds};
			howdy::pam::PreparedRuntimeFiles prepared;
			ok &= Expect(!howdy::pam::auth_helper_process::PrepareRuntimeAuthFiles(
			                 "alice", &prepared, SpawnOperations(&fake)),
			             "fake descriptor collision reaches spawn");
			ok &= Expect(fake.duplicate_fd_requests ==
			                 std::vector<std::pair<int, int>>{{fds.at(0), 3}, {fds.at(1), 3}},
			             "fake collision normalizes both pipe descriptors");
			ok &=
			    Expect(fake.dup2_fds == std::vector<std::pair<int, int>>{{21, 1}, {21, 2}, {13, 3}},
			           "fake collision uses normalized output fd and ordered lease fd");
			ok &= ExpectClosedExactlyOnce(fake, {fds.at(0), fds.at(1), 20, 21, 12, 13},
			                              "fake descriptor collision");
		}
		return ok;
	}

	struct ExecProbeContext {
		std::filesystem::path marker;
	};

	auto ExecProbeSpawn(const howdy::pam::auth_helper_process::SpawnRequest &request) -> int {
		const auto           &context = *static_cast<const ExecProbeContext *>(request.context);
		std::string           program("pam_runtime_session_test");
		std::string           mode("--fd3-probe");
		std::string           marker      = context.marker.string();
		std::array<char *, 4> arguments   = {program.data(), mode.data(), marker.data(), nullptr};
		std::array<char *, 1> environment = {nullptr};
		return posix_spawn(request.child_pid, "/proc/self/exe", request.actions, nullptr,
		                   arguments.data(), environment.data());
	}

	auto TestRealDescriptorCollision() -> bool {
		auto            marker = std::filesystem::temp_directory_path() /
		                         ("howdy-fd3-probe-" + std::to_string(getpid()));
		std::error_code error;
		std::filesystem::remove(marker, error);
		const pid_t pid = fork();
		if (pid == 0) {
			(void)close(STDOUT_FILENO);
			(void)close(STDERR_FILENO);
			ExecProbeContext context{.marker = marker};
			auto             operations = howdy::pam::auth_helper_process::ProductionOperations();
			operations.context          = &context;
			operations.spawn            = ExecProbeSpawn;
			howdy::pam::PreparedRuntimeFiles prepared;
			(void)howdy::pam::auth_helper_process::PrepareRuntimeAuthFiles(
			    "alice", &prepared, operations,
			    std::chrono::steady_clock::now() + std::chrono::seconds(2));
			_exit(std::filesystem::exists(marker) ? EXIT_SUCCESS : EXIT_FAILURE);
		}
		bool ok = Expect(pid >= 0, "real descriptor collision forks child");
		if (pid >= 0) {
			int status = 0;
			while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
			}
			ok &= Expect(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS,
			             "exec child retains fd 3 as AF_UNIX SOCK_SEQPACKET");
		}
		std::filesystem::remove(marker, error);
		return ok;
	}

	auto SendRightsMessage(int socket_fd, std::string_view marker, const std::vector<int> &fds)
	    -> bool {
		char              marker_byte = marker.empty() ? '\0' : marker.front();
		iovec             data{.iov_base = &marker_byte, .iov_len = marker.empty() ? 0U : 1U};
		std::vector<char> control(CMSG_SPACE(sizeof(int) * fds.size()));
		msghdr            message{};
		message.msg_iov        = &data;
		message.msg_iovlen     = 1;
		message.msg_control    = control.data();
		message.msg_controllen = control.size();
		cmsghdr *header        = CMSG_FIRSTHDR(&message);
		header->cmsg_level     = SOL_SOCKET;
		header->cmsg_type      = SCM_RIGHTS;
		header->cmsg_len       = CMSG_LEN(sizeof(int) * fds.size());
		if (!fds.empty()) {
			std::memcpy(CMSG_DATA(header), fds.data(), sizeof(int) * fds.size());
		}
		return sendmsg(socket_fd, &message, MSG_NOSIGNAL) == static_cast<ssize_t>(data.iov_len);
	}

	auto ReceiveCase(std::string_view name, char marker, std::size_t descriptor_count,
	                 bool expected) -> bool {
		std::array<int, 2> sockets{};
		if (!Expect(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets.data()) == 0,
		            std::string(name) + " creates socketpair")) {
			return false;
		}
		auto      path      = std::to_array("/tmp/howdy-lease-receive-XXXXXX");
		const int source_fd = mkstemp(path.data());
		if (source_fd < 0) {
			(void)close(sockets.at(0));
			(void)close(sockets.at(1));
			return Expect(false, std::string(name) + " creates source descriptor");
		}
		(void)unlink(path.data());
		std::vector<int> const fds(descriptor_count, source_fd);
		const bool             sent = descriptor_count == 0
		                                  ? send(sockets.at(1), &marker, 1, MSG_NOSIGNAL) == 1
		                                  : SendRightsMessage(sockets.at(1), {&marker, 1}, fds);
		int                    received_fd = -1;
		const bool             result =
		    sent && howdy::pam::auth_helper_process::internal::ReceiveLeaseDescriptorUntil(
		                sockets.at(0), &received_fd,
		                std::chrono::steady_clock::now() + std::chrono::seconds(1));
		bool ok = Expect(result == expected, std::string(name) + " has expected result");
		if (result) {
			struct stat source_stat{};
			struct stat received_stat{};
			ok &= Expect(fstat(source_fd, &source_stat) == 0 &&
			                 fstat(received_fd, &received_stat) == 0 &&
			                 source_stat.st_dev == received_stat.st_dev &&
			                 source_stat.st_ino == received_stat.st_ino,
			             std::string(name) + " receives exact descriptor");
			const int descriptor_flags = fcntl(received_fd, F_GETFD);
			ok &= Expect(descriptor_flags >= 0 &&
			                 howdy::native::HasAnyBits(descriptor_flags, FD_CLOEXEC),
			             std::string(name) + " receives close-on-exec descriptor");
		}
		if (received_fd >= 0) {
			(void)close(received_fd);
		}
		(void)close(source_fd);
		(void)close(sockets.at(0));
		(void)close(sockets.at(1));
		return ok;
	}

	auto TestLeaseReceiver() -> bool {
		bool ok = true;
		for (const std::string_view payload : {"", "L"}) {
			for (const std::size_t count : {1U, 2U, 32U}) {
				std::array<int, 2> sockets{};
				if (!Expect(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets.data()) ==
				                0,
				            "EOF rights creates socketpair")) {
					return false;
				}
				const howdy::test::ScopedFd receiver(sockets.at(0));
				howdy::test::ScopedFd       sender(sockets.at(1));
				const howdy::test::ScopedFd source(open("/dev/null", O_RDONLY | O_CLOEXEC));
				ok &= Expect(source.Get() >= 0, "EOF rights opens source");
				ok &= Expect(
				    SendRightsMessage(sender.Get(), payload, std::vector<int>(count, source.Get())),
				    "EOF rights sends ancillary data");
				const auto count_fds = [] -> std::ptrdiff_t {
					return std::distance(std::filesystem::directory_iterator("/proc/self/fd"),
					                     std::filesystem::directory_iterator{});
				};
				const auto before = count_fds();
				ok &= Expect(!howdy::pam::auth_helper_process::internal::LeaseSocketHasCleanEof(
				                 receiver.Get()),
				             "EOF rejects ancillary data");
				ok &=
				    Expect(count_fds() == before, "EOF rejection closes all received descriptors");
				sender.Reset();
				ok &= Expect(howdy::pam::auth_helper_process::internal::LeaseSocketHasCleanEof(
				                 receiver.Get()),
				             "EOF accepts clean shutdown");
			}
		}
		ok &= ReceiveCase("valid SCM_RIGHTS", 'L', 1, true);
		ok &= ReceiveCase("wrong marker", 'X', 1, false);
		ok &= ReceiveCase("missing descriptor", 'L', 0, false);
		ok &= ReceiveCase("multiple descriptors", 'L', 2, false);

		const int before_truncation = open("/dev/null", O_RDONLY | O_CLOEXEC);
		ok &= Expect(before_truncation >= 0, "truncation leak probe opens baseline descriptor");
		(void)close(before_truncation);
		ok &= ReceiveCase("truncated descriptors", 'L', 32, false);
		const int after_truncation = open("/dev/null", O_RDONLY | O_CLOEXEC);
		ok &= Expect(after_truncation == before_truncation,
		             "truncated descriptor rejection leaks no received descriptors");
		(void)close(after_truncation);

		std::array<int, 2> sockets{};
		int                received_fd = -1;
		ok &= Expect(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets.data()) == 0,
		             "lease EOF creates socketpair");
		(void)close(sockets.at(1));
		received_fd = -1;
		ok &= Expect(!howdy::pam::auth_helper_process::internal::ReceiveLeaseDescriptorUntil(
		                 sockets.at(0), &received_fd,
		                 std::chrono::steady_clock::now() + std::chrono::milliseconds(50)),
		             "lease EOF is rejected");
		(void)close(sockets.at(0));

		ok &= Expect(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets.data()) == 0,
		             "lease timeout creates socketpair");
		ok &= Expect(!howdy::pam::auth_helper_process::internal::ReceiveLeaseDescriptorUntil(
		                 sockets.at(0), &received_fd,
		                 std::chrono::steady_clock::now() + std::chrono::milliseconds(50)),
		             "missing lease times out");
		(void)close(sockets.at(0));
		(void)close(sockets.at(1));
		return ok;
	}

	auto TestLeaseValidation() -> bool {
		auto        directory_template = std::to_array("/tmp/howdy-lease-validation-XXXXXX");
		char const *parent             = mkdtemp(directory_template.data());
		if (!Expect(parent != nullptr, "lease validation creates parent")) {
			return false;
		}
		const auto generation = std::filesystem::path(parent) / "pam-1-gen000";
		const auto lock_path  = std::filesystem::path(generation.string() + ".lock");
		const int creator_fd = open(lock_path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		bool      ok         = Expect(creator_fd >= 0, "lease validation creates lock file");
		if (creator_fd >= 0) {
			ok &= Expect(!howdy::pam::auth_helper_process::internal::ValidateLeaseDescriptor(
			                 creator_fd, generation, geteuid()),
			             "lease validation rejects writable descriptor");
			(void)close(creator_fd);
		}
		const int lease_fd = open(lock_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
		ok &= Expect(lease_fd >= 0, "lease validation opens read-only descriptor");
		if (lease_fd >= 0) {
			ok &= Expect(howdy::native::MaskedBits(fcntl(lease_fd, F_GETFL), O_ACCMODE) == O_RDONLY,
			             "lease descriptor is read-only");
			errno = 0;
			ok &= Expect(write(lease_fd, "x", 1) == -1 && errno == EBADF,
			             "read-only lease rejects write");
			errno = 0;
			ok &= Expect(ftruncate(lease_fd, 1) == -1 && (errno == EINVAL || errno == EBADF),
			             "read-only lease rejects truncate");
			ok &= Expect(howdy::pam::auth_helper_process::internal::ValidateLeaseDescriptor(
			                 lease_fd, generation, geteuid()),
			             "lease validation accepts exact unlocked sibling and takes shared lock");
			const int independent_fd = open(lock_path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
			ok &= Expect(independent_fd >= 0, "lease validation opens independent lock descriptor");
			if (independent_fd >= 0) {
				errno = 0;
				ok &= Expect(flock(independent_fd, LOCK_EX | LOCK_NB) != 0 &&
				                 (errno == EWOULDBLOCK || errno == EAGAIN),
				             "validated lease blocks independent exclusive lock");
				(void)close(independent_fd);
			}

			const auto wrong_path = std::filesystem::path(parent) / "wrong.lock";
			const int  wrong_creator =
			    open(wrong_path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
			ok &= Expect(wrong_creator >= 0, "lease validation creates wrong-inode file");
			if (wrong_creator >= 0) {
				(void)close(wrong_creator);
			}
			const int wrong_fd = open(wrong_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
			ok &= Expect(wrong_fd >= 0, "lease validation creates wrong-inode descriptor");
			if (wrong_fd >= 0) {
				ok &= Expect(!howdy::pam::auth_helper_process::internal::ValidateLeaseDescriptor(
				                 wrong_fd, generation, geteuid()),
				             "lease validation rejects wrong inode");
				(void)close(wrong_fd);
			}
			(void)close(lease_fd);
		}
		std::error_code error;
		std::filesystem::remove_all(parent, error);
		return ok;
	}

	enum class PrepareExceptionPoint : std::uint8_t {
		kSetup,
		kSpawn,
		kCollect,
		kCollectLease,
		kWait
	};

	struct ReapContext {
		std::array<int, 2>       output_pipe  = {-1, -1};
		std::array<int, 2>       lease_socket = {-1, -1};
		std::filesystem::path    root;
		int                      lease_fd      = -1;
		int                      destroy_calls = 0;
		pid_t                    child_pid     = -1;
		std::vector<std::string> log_messages;
		PrepareExceptionPoint    exception_point   = PrepareExceptionPoint::kSetup;
		bool                     lease_received    = false;
		bool                     reaped_before_log = false;
	};

	auto RecordingPipe2(void *context, int *fds, int flags) -> int {
		auto      &state       = *static_cast<ReapContext *>(context);
		const int  result      = pipe2(fds, flags);
		const auto descriptors = std::span(fds, std::size_t{2});
		state.output_pipe      = {descriptors.front(), descriptors.back()};
		return result;
	}

	auto RecordingSocketpair(void *context, int domain, int type, int protocol, int *fds) -> int {
		auto      &state       = *static_cast<ReapContext *>(context);
		const int  result      = socketpair(domain, type, protocol, fds);
		const auto descriptors = std::span(fds, std::size_t{2});
		state.lease_socket     = {descriptors.front(), descriptors.back()};
		return result;
	}

	auto ValidSpawn(const howdy::pam::auth_helper_process::SpawnRequest &request) -> int {
		auto       &state = *static_cast<ReapContext *>(request.context);
		const pid_t pid   = fork();
		if (pid < 0) {
			return errno;
		}
		if (pid == 0) {
			const std::string output = "CONFIG_PATH=" + (state.root / "config.ini").string() +
			                           "\nUSER_MODELS_DIR=" + (state.root / "models").string() +
			                           "\n";
			const bool output_ok = write(state.output_pipe.at(1), output.data(), output.size()) ==
			                       static_cast<ssize_t>(output.size());
			const bool lease_ok =
			    SendRightsMessage(state.lease_socket.at(1), "L", {state.lease_fd});
			(void)close(state.output_pipe.at(0));
			(void)close(state.output_pipe.at(1));
			(void)close(state.lease_socket.at(0));
			(void)close(state.lease_socket.at(1));
			(void)close(state.lease_fd);
			_exit(output_ok && lease_ok ? EXIT_SUCCESS : EXIT_FAILURE);
		}
		*request.child_pid = pid;
		state.child_pid    = pid;
		return 0;
	}

	auto FailingActionsDestroy(void *context, posix_spawn_file_actions_t *actions) -> int {
		auto &state = *static_cast<ReapContext *>(context);
		++state.destroy_calls;
		(void)posix_spawn_file_actions_destroy(actions);
		return EIO;
	}

	void ReapLog(void *context, std::string_view message) {
		static_cast<ReapContext *>(context)->log_messages.emplace_back(message);
	}

	auto TestValidPrepareIgnoresDestroyFailure() -> bool {
		if (geteuid() != 0) {
			return true;
		}
		ReapContext state;
		state.root = howdy::native::auth_helper_protocol::PreparedRuntimeGenerationDir(
		    howdy::native::auth_helper_protocol::PreparedRuntimeRoot(), 0,
		    howdy::native::auth_helper_protocol::RuntimeGenerationSlot::kSlot0);
		std::error_code error;
		std::filesystem::create_directories(state.root / "models", error);
		const auto lock_path = std::filesystem::path(state.root.string() + ".lock");
		const int creator_fd = open(lock_path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		bool      ok = Expect(!error && creator_fd >= 0, "valid prepare creates runtime lease");
		if (creator_fd >= 0) {
			(void)close(creator_fd);
			state.lease_fd = open(lock_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
			ok &= Expect(state.lease_fd >= 0, "valid prepare opens read-only runtime lease");
		}
		if (state.lease_fd >= 0) {
			auto operations            = howdy::pam::auth_helper_process::ProductionOperations();
			operations.context         = &state;
			operations.pipe2           = RecordingPipe2;
			operations.socketpair      = RecordingSocketpair;
			operations.actions_destroy = FailingActionsDestroy;
			operations.spawn           = ValidSpawn;
			operations.log_observer    = ReapLog;
			howdy::pam::PreparedRuntimeFiles prepared;
			ok &= Expect(howdy::pam::auth_helper_process::PrepareRuntimeAuthFiles(
			                 "alice", &prepared, operations,
			                 std::chrono::steady_clock::now() + std::chrono::seconds(1)),
			             "valid output and lease survive post-spawn destroy failure");
			ok &= Expect(state.destroy_calls == 1 && prepared.root_dir == state.root,
			             "valid prepare preserves paths and destroys actions once");
			ok &= Expect(state.log_messages.size() == 1 &&
			                 state.log_messages.at(0).contains("posix_spawn_file_actions_destroy"),
			             "valid prepare logs post-spawn destroy failure");
			prepared.lease_fd.Reset();
			(void)close(state.lease_fd);
		}
		std::filesystem::remove_all(state.root, error);
		std::filesystem::remove(lock_path, error);
		return ok;
	}

	auto EofSpawn(const howdy::pam::auth_helper_process::SpawnRequest &request) -> int {
		auto       &state = *static_cast<ReapContext *>(request.context);
		const pid_t pid   = fork();
		if (pid < 0) {
			return errno;
		}
		if (pid == 0) {
			(void)close(state.output_pipe.at(0));
			(void)close(state.lease_socket.at(0));
			(void)close(state.output_pipe.at(1));
			(void)close(state.lease_socket.at(1));
			_exit(EXIT_SUCCESS);
		}
		*request.child_pid = pid;
		state.child_pid    = pid;
		return 0;
	}

	auto TestPrepareFailureReapsChild() -> bool {
		ReapContext state;
		auto        operations = howdy::pam::auth_helper_process::ProductionOperations();
		operations.context     = &state;
		operations.pipe2       = RecordingPipe2;
		operations.socketpair  = RecordingSocketpair;
		operations.spawn       = EofSpawn;
		howdy::pam::PreparedRuntimeFiles prepared;
		const bool result = howdy::pam::auth_helper_process::PrepareRuntimeAuthFiles(
		    "alice", &prepared, operations,
		    std::chrono::steady_clock::now() + std::chrono::seconds(1));
		errno                   = 0;
		const pid_t wait_result = waitpid(state.child_pid, nullptr, WNOHANG);
		bool        ok          = Expect(!result, "missing lease prepare fails");
		ok &= Expect(wait_result < 0 && errno == ECHILD, "missing lease child is already reaped");
		return ok;
	}

	auto ExceptionSpawn(const howdy::pam::auth_helper_process::SpawnRequest &request) -> int {
		auto &state = *static_cast<ReapContext *>(request.context);
		if (state.lease_fd >= 0) {
			if (flock(state.lease_fd, LOCK_SH) != 0 ||
			    !SendRightsMessage(state.lease_socket.at(1), "L", {state.lease_fd})) {
				return EIO;
			}
			(void)close(state.lease_fd);
			state.lease_fd = -1;
		}
		const pid_t pid = fork();
		if (pid < 0) {
			return errno;
		}
		if (pid == 0) {
			(void)alarm(5);
			if (state.exception_point == PrepareExceptionPoint::kWait) {
				for (const int fd : state.output_pipe) {
					(void)close(fd);
				}
				for (const int fd : state.lease_socket) {
					(void)close(fd);
				}
			}
			for (;;) {
				pause();
			}
		}
		*request.child_pid = pid;
		state.child_pid    = pid;
		if (state.exception_point == PrepareExceptionPoint::kSpawn) {
			throw std::runtime_error("injected exception after child creation");
		}
		return 0;
	}

	void ExceptionTimeoutLog(void *context, std::string_view message) {
		(void)message;
		auto  &state = *static_cast<ReapContext *>(context);
		pollfd socket{.fd = state.lease_socket.at(0), .events = POLLIN, .revents = 0};
		state.lease_received =
		    poll(&socket, 1, 0) >= 0 && !howdy::native::HasAnyBits(socket.revents, POLLIN);
		errno                   = 0;
		state.reaped_before_log = waitpid(state.child_pid, nullptr, WNOHANG) < 0 && errno == ECHILD;
		if (state.exception_point == PrepareExceptionPoint::kWait) {
			// Waiting is reached only after the collector receives the lease.
			state.lease_received = state.reaped_before_log;
		}
		throw std::runtime_error("injected timeout observer exception");
	}

	auto TestPrepareExceptionCleanup(PrepareExceptionPoint point) -> bool {
		ReapContext state;
		state.exception_point = point;
		const bool with_lease =
		    point == PrepareExceptionPoint::kCollectLease || point == PrepareExceptionPoint::kWait;
		howdy::native::ScopedFd independent;
		if (with_lease) {
			auto path      = std::to_array("/tmp/howdy-exception-lease-XXXXXX");
			state.lease_fd = mkstemp(path.data());
			if (!Expect(state.lease_fd >= 0, "exception test creates lease")) {
				return false;
			}
			independent.Reset(open(path.data(), O_RDWR | O_CLOEXEC));
			(void)unlink(path.data());
			if (!Expect(independent.Valid(), "exception test opens independent lease probe")) {
				(void)close(state.lease_fd);
				return false;
			}
		}
		auto operations            = howdy::pam::auth_helper_process::ProductionOperations();
		operations.context         = &state;
		operations.pipe2           = RecordingPipe2;
		operations.socketpair      = RecordingSocketpair;
		operations.spawn           = ExceptionSpawn;
		operations.log_observer    = ExceptionTimeoutLog;
		operations.actions_destroy = [](void *context, posix_spawn_file_actions_t *actions) -> int {
			++static_cast<ReapContext *>(context)->destroy_calls;
			return posix_spawn_file_actions_destroy(actions);
		};
		if (point == PrepareExceptionPoint::kSetup) {
			operations.actions_adddup2 = [](void *, posix_spawn_file_actions_t *, int, int) -> int {
				throw std::runtime_error("injected setup exception");
			};
		}
		bool                             caught = false;
		howdy::pam::PreparedRuntimeFiles prepared;
		try {
			(void)howdy::pam::auth_helper_process::PrepareRuntimeAuthFiles(
			    "alice", &prepared, operations,
			    std::chrono::steady_clock::now() + std::chrono::milliseconds(150));
		} catch (const std::runtime_error &) {
			caught = true;
		}
		bool ok = Expect(caught, "prepare propagates injected exception after cleanup");
		ok &=
		    Expect(state.destroy_calls == 1, "exception destroys initialized actions exactly once");
		for (const int fd : {state.output_pipe.at(0), state.output_pipe.at(1),
		                     state.lease_socket.at(0), state.lease_socket.at(1)}) {
			errno = 0;
			ok &= Expect(fcntl(fd, F_GETFD) < 0 && errno == EBADF,
			             "exception closes channel descriptor");
		}
		if (state.child_pid > 0) {
			errno                   = 0;
			const pid_t wait_result = waitpid(state.child_pid, nullptr, WNOHANG);
			const bool  reaped      = wait_result < 0 && errno == ECHILD;
			ok &= Expect(reaped, "exception leaves no helper child to reap");
			if (wait_result == 0) {
				(void)kill(state.child_pid, SIGKILL);
				while (waitpid(state.child_pid, nullptr, 0) < 0 && errno == EINTR) {
				}
			}
		}
		if (with_lease) {
			ok &= Expect(state.lease_received, "collector received queued lease before exception");
			ok &= Expect(flock(independent.Get(), LOCK_EX | LOCK_NB) == 0,
			             "exception releases received shared lease");
		}
		if (point == PrepareExceptionPoint::kWait) {
			ok &= Expect(state.reaped_before_log,
			             "wait consumes child before throwing timeout logger");
		}
		if (state.lease_fd >= 0) {
			(void)close(state.lease_fd);
		}
		return ok;
	}

}  // namespace

auto RunRuntimeSessionFd3Probe(const char *marker_path) -> int {
	int       socket_type = 0;
	socklen_t type_length = sizeof(socket_type);
	if (getsockopt(3, SOL_SOCKET, SO_TYPE, &socket_type, &type_length) != 0 ||
	    socket_type != SOCK_SEQPACKET) {
		return EXIT_FAILURE;
	}

	sockaddr  address{};
	socklen_t address_length = sizeof(address);
	if (getsockname(3, &address, &address_length) != 0 || address.sa_family != AF_UNIX) {
		return EXIT_FAILURE;
	}

	const int marker_fd = open(marker_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (marker_fd < 0) {
		return EXIT_FAILURE;
	}
	return close(marker_fd) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

auto RunRuntimeSessionSpawnTests() -> bool {
	bool ok = true;
	ok &= TestAuthHelperSpawnSetup();
	ok &= TestDescriptorCollisions();
	ok &= TestRealDescriptorCollision();
	ok &= TestLeaseReceiver();
	ok &= TestLeaseValidation();
	ok &= TestValidPrepareIgnoresDestroyFailure();
	ok &= TestPrepareFailureReapsChild();
	for (const auto point : {PrepareExceptionPoint::kSetup, PrepareExceptionPoint::kSpawn,
	                         PrepareExceptionPoint::kCollect, PrepareExceptionPoint::kCollectLease,
	                         PrepareExceptionPoint::kWait}) {
		ok &= TestPrepareExceptionCleanup(point);
	}
	return ok;
}
