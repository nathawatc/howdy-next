#include "runtime/runtime_session_process_test_support.hpp"
#include "runtime/runtime_session_test_groups.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fcntl.h>

#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>

namespace {
	using namespace howdy::test::runtime_session;

	auto test_auth_helper_spawn_setup() -> bool {
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
			const bool result = howdy::pam::auth_helper_process::prepare_runtime_auth_files(
			    "alice", &prepared, spawn_operations(&fake));
			ok &= expect(!result, failure.operation + " setup failure is rejected");
			ok &=
			    expect(fake.spawn_calls == 0, failure.operation + " setup failure does not spawn");
			ok &= expect(fake.destroy_calls == failure.expected_destroy_calls,
			             failure.operation + " has exact action destroy count");
			ok &= expect_closed_exactly_once(fake, failure.expected_closed_fds, failure.operation);
			const auto failed = std::ranges::find(setup_order, failure.operation);
			ok &= expect_operation_prefix(
			    fake,
			    std::vector<std::string>(setup_order.begin(), failed == setup_order.end()
			                                                      ? setup_order.end()
			                                                      : failed + 1),
			    failure.operation);
		}

		AuthHelperSpawnFake              setup_destroy_failure{.fail_operation = "dup_stdout",
		                                                       .destroy_fails  = true};
		howdy::pam::PreparedRuntimeFiles setup_ignored;
		ok &= expect(!howdy::pam::auth_helper_process::prepare_runtime_auth_files(
		                 "alice", &setup_ignored, spawn_operations(&setup_destroy_failure)),
		             "combined setup and destroy failure is rejected");
		ok &=
		    expect_log(setup_destroy_failure, 0, "posix_spawn_file_actions_adddup2(STDOUT_FILENO)",
		               "combined setup and destroy failure");
		ok &= expect_log(setup_destroy_failure, 1, "posix_spawn_file_actions_destroy",
		                 "combined setup and destroy failure");

		AuthHelperSpawnFake              fake{.fail_operation = "spawn"};
		howdy::pam::PreparedRuntimeFiles prepared;
		const bool result = howdy::pam::auth_helper_process::prepare_runtime_auth_files(
		    "alice", &prepared, spawn_operations(&fake));
		ok &= expect(!result, "spawn failure is rejected");
		ok &= expect_log(fake, 0, "posix_spawn", "spawn failure");
		ok &= expect(fake.spawn_calls == 1, "spawn failure attempts spawn once");
		ok &= expect(fake.destroy_calls == 1, "spawn failure destroys actions once");
		ok &= expect_closed_exactly_once(fake, {10, 11, 12, 13}, "spawn failure");
		ok &=
		    expect(fake.pipe_flags == std::vector<int>{O_CLOEXEC}, "output pipe is close-on-exec");
		ok &=
		    expect(fake.socket_parameters ==
		               std::vector<std::array<int, 3>>{{AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0}},
		           "lease socketpair is sequenced-packet and close-on-exec");
		ok &= expect(fake.dup2_fds == std::vector<std::pair<int, int>>{{11, STDOUT_FILENO},
		                                                               {11, STDERR_FILENO},
		                                                               {13, 3}},
		             "child receives output and lease fd 3");
		ok &= expect(fake.action_closefrom_fds == std::vector<int>{4},
		             "child closes descriptors from fd 4");
		ok &= expect(fake.action_close_fds == std::vector<int>{10, 11, 12, 13},
		             "child closes original pipe and socket descriptors");
		ok &= expect(fake.spawn_argv ==
		                     std::vector<std::string>{kAuthHelperPath, "prepare", "alice"} &&
		                 fake.spawn_env.empty(),
		             "prepare uses exact argv and empty environment");
		ok &= expect(fake.operations ==
		                 std::vector<std::string>{
		                     "pipe2", "socketpair", "actions_init", "close_output_read",
		                     "dup_stdout", "dup_stderr", "close_output_write", "close_lease_parent",
		                     "dup_lease", "close_lease_child", "closefrom", "spawn", "destroy",
		                     "close", "close", "close", "close"},
		             "prepare owns actions and descriptors in order");

		AuthHelperSpawnFake spawn_destroy_failure{.fail_operation = "spawn", .destroy_fails = true};
		howdy::pam::PreparedRuntimeFiles spawn_ignored;
		ok &= expect(!howdy::pam::auth_helper_process::prepare_runtime_auth_files(
		                 "alice", &spawn_ignored, spawn_operations(&spawn_destroy_failure)),
		             "combined spawn and destroy failure is rejected");
		ok &= expect_log(spawn_destroy_failure, 0, "posix_spawn",
		                 "combined spawn and destroy failure");
		ok &= expect_log(spawn_destroy_failure, 1, "posix_spawn_file_actions_destroy",
		                 "combined spawn and destroy failure");

		AuthHelperSpawnFake              destroy_failure{.destroy_fails = true};
		howdy::pam::PreparedRuntimeFiles ignored;
		ok &= expect(!howdy::pam::auth_helper_process::prepare_runtime_auth_files(
		                 "alice", &ignored, spawn_operations(&destroy_failure),
		                 std::chrono::steady_clock::now() + std::chrono::seconds(1)),
		             "child EOF still rejects prepare after destroy failure");
		ok &= expect(destroy_failure.destroy_calls == 1 && destroy_failure.spawned_pid > 0,
		             "post-spawn destroy failure preserves child lifecycle");
		ok &= expect_log(destroy_failure, 0, "posix_spawn_file_actions_destroy", "destroy failure");
		errno = 0;
		ok &= expect(waitpid(destroy_failure.spawned_pid, nullptr, WNOHANG) < 0 && errno == ECHILD,
		             "destroy failure child is reaped");
		return ok;
	}

	auto test_descriptor_collisions() -> bool {
		AuthHelperSpawnFake              duplicate_failure{.fail_operation = "duplicate_fd",
		                                                   .next_pipe_fds  = {STDOUT_FILENO, 11}};
		howdy::pam::PreparedRuntimeFiles failed_prepare;
		bool ok = expect(!howdy::pam::auth_helper_process::prepare_runtime_auth_files(
		                     "alice", &failed_prepare, spawn_operations(&duplicate_failure)),
		                 "descriptor normalization failure is rejected");
		ok &= expect_closed_exactly_once(duplicate_failure, {STDOUT_FILENO, 11},
		                                 "descriptor normalization failure");
		AuthHelperSpawnFake lease_collision{.fail_operation = "spawn", .next_socket_fds = {3, 12}};
		howdy::pam::PreparedRuntimeFiles lease_prepare;
		ok &= expect(!howdy::pam::auth_helper_process::prepare_runtime_auth_files(
		                 "alice", &lease_prepare, spawn_operations(&lease_collision)),
		             "lease fd 3 collision reaches spawn");
		ok &= expect(lease_collision.duplicate_fd_requests ==
		                 std::vector<std::pair<int, int>>{{3, 4}},
		             "lease fd 3 is normalized above reserved target");
		ok &= expect(lease_collision.dup2_fds ==
		                 std::vector<std::pair<int, int>>{{11, 1}, {11, 2}, {12, 3}},
		             "normalized lease parent cannot close child fd 3");
		ok &= expect_closed_exactly_once(lease_collision, {3, 20, 10, 11, 12},
		                                 "lease fd 3 collision");

		for (const auto fds :
		     {std::array{STDOUT_FILENO, STDERR_FILENO}, std::array{STDERR_FILENO, STDOUT_FILENO}}) {
			AuthHelperSpawnFake              fake{.fail_operation = "spawn", .next_pipe_fds = fds};
			howdy::pam::PreparedRuntimeFiles prepared;
			ok &= expect(!howdy::pam::auth_helper_process::prepare_runtime_auth_files(
			                 "alice", &prepared, spawn_operations(&fake)),
			             "fake descriptor collision reaches spawn");
			ok &= expect(fake.duplicate_fd_requests ==
			                 std::vector<std::pair<int, int>>{{fds[0], 3}, {fds[1], 3}},
			             "fake collision normalizes both pipe descriptors");
			ok &=
			    expect(fake.dup2_fds == std::vector<std::pair<int, int>>{{21, 1}, {21, 2}, {13, 3}},
			           "fake collision uses normalized output fd and ordered lease fd");
			ok &= expect_closed_exactly_once(fake, {fds[0], fds[1], 20, 21, 12, 13},
			                                 "fake descriptor collision");
		}
		return ok;
	}

	struct ExecProbeContext {
		std::filesystem::path marker;
	};

	auto exec_probe_spawn(const howdy::pam::auth_helper_process::SpawnRequest &request) -> int {
		const auto           &context = *static_cast<const ExecProbeContext *>(request.context);
		std::string           marker  = context.marker.string();
		std::array<char *, 4> arguments = {const_cast<char *>("pam_runtime_session_test"),
		                                   const_cast<char *>("--fd3-probe"), marker.data(), nullptr};
		std::array<char *, 1> environment = {nullptr};
		return posix_spawn(request.child_pid, "/proc/self/exe", request.actions, nullptr,
		                   arguments.data(), environment.data());
	}

	auto test_real_descriptor_collision() -> bool {
		auto            marker = std::filesystem::temp_directory_path() /
		                         ("howdy-fd3-probe-" + std::to_string(getpid()));
		std::error_code error;
		std::filesystem::remove(marker, error);
		const pid_t pid = fork();
		if (pid == 0) {
			(void)close(STDOUT_FILENO);
			(void)close(STDERR_FILENO);
			ExecProbeContext context{.marker = marker};
			auto             operations = howdy::pam::auth_helper_process::production_operations();
			operations.context          = &context;
			operations.spawn            = exec_probe_spawn;
			howdy::pam::PreparedRuntimeFiles prepared;
			(void)howdy::pam::auth_helper_process::prepare_runtime_auth_files(
			    "alice", &prepared, operations,
			    std::chrono::steady_clock::now() + std::chrono::seconds(2));
			_exit(std::filesystem::exists(marker) ? EXIT_SUCCESS : EXIT_FAILURE);
		}
		bool ok = expect(pid >= 0, "real descriptor collision forks child");
		if (pid >= 0) {
			int status = 0;
			while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
			}
			ok &= expect(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS,
			             "exec child retains fd 3 as AF_UNIX SOCK_SEQPACKET");
		}
		std::filesystem::remove(marker, error);
		return ok;
	}

	auto send_rights_message(int socket_fd, std::string_view marker, const std::vector<int> &fds)
	    -> bool {
		char              marker_byte = marker.front();
		iovec             data{.iov_base = &marker_byte, .iov_len = sizeof(marker_byte)};
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
		return sendmsg(socket_fd, &message, MSG_NOSIGNAL) == 1;
	}

	auto receive_case(std::string_view name, char marker, std::size_t descriptor_count,
	                  bool expected) -> bool {
		std::array<int, 2> sockets{};
		if (!expect(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets.data()) == 0,
		            std::string(name) + " creates socketpair")) {
			return false;
		}
		auto      path      = std::to_array("/tmp/howdy-lease-receive-XXXXXX");
		const int source_fd = mkstemp(path.data());
		if (source_fd < 0) {
			(void)close(sockets[0]);
			(void)close(sockets[1]);
			return expect(false, std::string(name) + " creates source descriptor");
		}
		(void)unlink(path.data());
		std::vector<int> fds(descriptor_count, source_fd);
		const bool       sent        = descriptor_count == 0
		                                   ? send(sockets[1], &marker, 1, MSG_NOSIGNAL) == 1
		                                   : send_rights_message(sockets[1], {&marker, 1}, fds);
		int              received_fd = -1;
		const bool       result = sent && howdy::pam::auth_helper_process::receive_lease_descriptor(
		                                      sockets[0], &received_fd,
		                                      std::chrono::steady_clock::now() + std::chrono::seconds(1));
		bool ok = expect(result == expected, std::string(name) + " has expected result");
		if (result) {
			struct stat source_stat{};
			struct stat received_stat{};
			ok &= expect(fstat(source_fd, &source_stat) == 0 &&
			                 fstat(received_fd, &received_stat) == 0 &&
			                 source_stat.st_dev == received_stat.st_dev &&
			                 source_stat.st_ino == received_stat.st_ino,
			             std::string(name) + " receives exact descriptor");
			ok &= expect((fcntl(received_fd, F_GETFD) & FD_CLOEXEC) != 0,
			             std::string(name) + " receives close-on-exec descriptor");
		}
		if (received_fd >= 0) {
			(void)close(received_fd);
		}
		(void)close(source_fd);
		(void)close(sockets[0]);
		(void)close(sockets[1]);
		return ok;
	}

	auto test_lease_receiver() -> bool {
		bool ok = true;
		ok &= receive_case("valid SCM_RIGHTS", 'L', 1, true);
		ok &= receive_case("wrong marker", 'X', 1, false);
		ok &= receive_case("missing descriptor", 'L', 0, false);
		ok &= receive_case("multiple descriptors", 'L', 2, false);

		const int before_truncation = open("/dev/null", O_RDONLY | O_CLOEXEC);
		ok &= expect(before_truncation >= 0, "truncation leak probe opens baseline descriptor");
		(void)close(before_truncation);
		ok &= receive_case("truncated descriptors", 'L', 32, false);
		const int after_truncation = open("/dev/null", O_RDONLY | O_CLOEXEC);
		ok &= expect(after_truncation == before_truncation,
		             "truncated descriptor rejection leaks no received descriptors");
		(void)close(after_truncation);

		std::array<int, 2> sockets{};
		int                received_fd = -1;
		ok &= expect(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets.data()) == 0,
		             "lease EOF creates socketpair");
		(void)close(sockets[1]);
		received_fd = -1;
		ok &= expect(!howdy::pam::auth_helper_process::receive_lease_descriptor(
		                 sockets[0], &received_fd,
		                 std::chrono::steady_clock::now() + std::chrono::milliseconds(50)),
		             "lease EOF is rejected");
		(void)close(sockets[0]);

		ok &= expect(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets.data()) == 0,
		             "lease timeout creates socketpair");
		ok &= expect(!howdy::pam::auth_helper_process::receive_lease_descriptor(
		                 sockets[0], &received_fd,
		                 std::chrono::steady_clock::now() + std::chrono::milliseconds(50)),
		             "missing lease times out");
		(void)close(sockets[0]);
		(void)close(sockets[1]);
		return ok;
	}

	auto test_lease_validation() -> bool {
		auto  directory_template = std::to_array("/tmp/howdy-lease-validation-XXXXXX");
		char *parent             = mkdtemp(directory_template.data());
		if (!expect(parent != nullptr, "lease validation creates parent")) {
			return false;
		}
		const auto generation = std::filesystem::path(parent) / "pam-1-gen000";
		const auto lock_path  = std::filesystem::path(generation.string() + ".lock");
		const int creator_fd = open(lock_path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		bool      ok         = expect(creator_fd >= 0, "lease validation creates lock file");
		if (creator_fd >= 0) {
			ok &= expect(!howdy::pam::auth_helper_process::validate_lease_descriptor(
			                 creator_fd, generation, geteuid()),
			             "lease validation rejects writable descriptor");
			(void)close(creator_fd);
		}
		const int lease_fd = open(lock_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
		ok &= expect(lease_fd >= 0, "lease validation opens read-only descriptor");
		if (lease_fd >= 0) {
			ok &= expect((fcntl(lease_fd, F_GETFL) & O_ACCMODE) == O_RDONLY,
			             "lease descriptor is read-only");
			errno = 0;
			ok &= expect(write(lease_fd, "x", 1) == -1 && errno == EBADF,
			             "read-only lease rejects write");
			errno = 0;
			ok &= expect(ftruncate(lease_fd, 1) == -1 && (errno == EINVAL || errno == EBADF),
			             "read-only lease rejects truncate");
			ok &= expect(howdy::pam::auth_helper_process::validate_lease_descriptor(
			                 lease_fd, generation, geteuid()),
			             "lease validation accepts exact unlocked sibling and takes shared lock");
			const int independent_fd = open(lock_path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
			ok &= expect(independent_fd >= 0, "lease validation opens independent lock descriptor");
			if (independent_fd >= 0) {
				errno = 0;
				ok &= expect(flock(independent_fd, LOCK_EX | LOCK_NB) != 0 &&
				                 (errno == EWOULDBLOCK || errno == EAGAIN),
				             "validated lease blocks independent exclusive lock");
				(void)close(independent_fd);
			}

			const auto wrong_path = std::filesystem::path(parent) / "wrong.lock";
			const int  wrong_creator =
			    open(wrong_path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
			ok &= expect(wrong_creator >= 0, "lease validation creates wrong-inode file");
			if (wrong_creator >= 0) {
				(void)close(wrong_creator);
			}
			const int wrong_fd = open(wrong_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
			ok &= expect(wrong_fd >= 0, "lease validation creates wrong-inode descriptor");
			if (wrong_fd >= 0) {
				ok &= expect(!howdy::pam::auth_helper_process::validate_lease_descriptor(
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

	struct ReapContext {
		std::array<int, 2>       output_pipe  = {-1, -1};
		std::array<int, 2>       lease_socket = {-1, -1};
		std::filesystem::path    root;
		int                      lease_fd      = -1;
		int                      destroy_calls = 0;
		pid_t                    child_pid     = -1;
		std::vector<std::string> log_messages;
	};

	auto recording_pipe2(void *context, int *fds, int flags) -> int {
		auto     &state   = *static_cast<ReapContext *>(context);
		const int result  = pipe2(fds, flags);
		state.output_pipe = {fds[0], fds[1]};
		return result;
	}

	auto recording_socketpair(void *context, int domain, int type, int protocol, int *fds) -> int {
		auto     &state    = *static_cast<ReapContext *>(context);
		const int result   = socketpair(domain, type, protocol, fds);
		state.lease_socket = {fds[0], fds[1]};
		return result;
	}

	auto valid_spawn(const howdy::pam::auth_helper_process::SpawnRequest &request) -> int {
		auto       &state = *static_cast<ReapContext *>(request.context);
		const pid_t pid   = fork();
		if (pid < 0) {
			return errno;
		}
		if (pid == 0) {
			const std::string output = "CONFIG_PATH=" + (state.root / "config.ini").string() +
			                           "\nUSER_MODELS_DIR=" + (state.root / "models").string() +
			                           "\n";
			const bool output_ok     = write(state.output_pipe[1], output.data(), output.size()) ==
			                           static_cast<ssize_t>(output.size());
			const bool lease_ok = send_rights_message(state.lease_socket[1], "L", {state.lease_fd});
			(void)close(state.output_pipe[0]);
			(void)close(state.output_pipe[1]);
			(void)close(state.lease_socket[0]);
			(void)close(state.lease_socket[1]);
			(void)close(state.lease_fd);
			_exit(output_ok && lease_ok ? EXIT_SUCCESS : EXIT_FAILURE);
		}
		*request.child_pid = pid;
		state.child_pid    = pid;
		return 0;
	}

	auto failing_actions_destroy(void *context, posix_spawn_file_actions_t *actions) -> int {
		auto &state = *static_cast<ReapContext *>(context);
		++state.destroy_calls;
		(void)posix_spawn_file_actions_destroy(actions);
		return EIO;
	}

	void reap_log(void *context, std::string_view message) {
		static_cast<ReapContext *>(context)->log_messages.emplace_back(message);
	}

	auto test_valid_prepare_ignores_destroy_failure() -> bool {
		if (geteuid() != 0) {
			return true;
		}
		ReapContext state;
		state.root = howdy::native::auth_helper_protocol::prepared_runtime_root() /
		             ("pam-test-" + std::to_string(getpid()));
		std::error_code error;
		std::filesystem::create_directories(state.root / "models", error);
		const auto lock_path = std::filesystem::path(state.root.string() + ".lock");
		state.lease_fd       = open(lock_path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		bool ok = expect(!error && state.lease_fd >= 0, "valid prepare creates runtime lease");
		if (state.lease_fd >= 0) {
			auto operations            = howdy::pam::auth_helper_process::production_operations();
			operations.context         = &state;
			operations.pipe2           = recording_pipe2;
			operations.socketpair      = recording_socketpair;
			operations.actions_destroy = failing_actions_destroy;
			operations.spawn           = valid_spawn;
			operations.log_observer    = reap_log;
			howdy::pam::PreparedRuntimeFiles prepared;
			ok &= expect(howdy::pam::auth_helper_process::prepare_runtime_auth_files(
			                 "alice", &prepared, operations,
			                 std::chrono::steady_clock::now() + std::chrono::seconds(1)),
			             "valid output and lease survive post-spawn destroy failure");
			ok &= expect(state.destroy_calls == 1 && prepared.root_dir == state.root,
			             "valid prepare preserves paths and destroys actions once");
			ok &= expect(state.log_messages.size() == 1 &&
			                 state.log_messages[0].contains("posix_spawn_file_actions_destroy"),
			             "valid prepare logs post-spawn destroy failure");
			if (prepared.lease_fd >= 0) {
				(void)close(prepared.lease_fd);
			}
			(void)close(state.lease_fd);
		}
		std::filesystem::remove_all(state.root, error);
		std::filesystem::remove(lock_path, error);
		return ok;
	}

	auto eof_spawn(const howdy::pam::auth_helper_process::SpawnRequest &request) -> int {
		auto       &state = *static_cast<ReapContext *>(request.context);
		const pid_t pid   = fork();
		if (pid < 0) {
			return errno;
		}
		if (pid == 0) {
			(void)close(state.output_pipe[0]);
			(void)close(state.lease_socket[0]);
			(void)close(state.output_pipe[1]);
			(void)close(state.lease_socket[1]);
			_exit(EXIT_SUCCESS);
		}
		*request.child_pid = pid;
		state.child_pid    = pid;
		return 0;
	}

	auto test_prepare_failure_reaps_child() -> bool {
		ReapContext state;
		auto        operations = howdy::pam::auth_helper_process::production_operations();
		operations.context     = &state;
		operations.pipe2       = recording_pipe2;
		operations.socketpair  = recording_socketpair;
		operations.spawn       = eof_spawn;
		howdy::pam::PreparedRuntimeFiles prepared;
		const bool result = howdy::pam::auth_helper_process::prepare_runtime_auth_files(
		    "alice", &prepared, operations,
		    std::chrono::steady_clock::now() + std::chrono::seconds(1));
		errno                   = 0;
		const pid_t wait_result = waitpid(state.child_pid, nullptr, WNOHANG);
		bool        ok          = expect(!result, "missing lease prepare fails");
		ok &= expect(wait_result < 0 && errno == ECHILD, "missing lease child is already reaped");
		return ok;
	}

}  // namespace

auto run_runtime_session_fd3_probe(const char *marker_path) -> int {
	int       socket_type = 0;
	socklen_t type_length = sizeof(socket_type);
	if (getsockopt(3, SOL_SOCKET, SO_TYPE, &socket_type, &type_length) != 0 ||
	    socket_type != SOCK_SEQPACKET) {
		return EXIT_FAILURE;
	}

	sockaddr_un address{};
	socklen_t   address_length = sizeof(address);
	if (getsockname(3, reinterpret_cast<sockaddr *>(&address), &address_length) != 0 ||
	    address.sun_family != AF_UNIX) {
		return EXIT_FAILURE;
	}

	const int marker_fd = open(marker_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (marker_fd < 0) {
		return EXIT_FAILURE;
	}
	return close(marker_fd) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

auto run_runtime_session_spawn_tests() -> bool {
	bool ok = true;
	ok &= test_auth_helper_spawn_setup();
	ok &= test_descriptor_collisions();
	ok &= test_real_descriptor_collision();
	ok &= test_lease_receiver();
	ok &= test_lease_validation();
	ok &= test_valid_prepare_ignores_destroy_failure();
	ok &= test_prepare_failure_reaps_child();
	return ok;
}
