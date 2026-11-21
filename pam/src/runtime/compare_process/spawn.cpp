#ifndef _GNU_SOURCE
#	define _GNU_SOURCE
#endif

#include "../child_process/internal.hpp"
#include "paths.hpp"
#include "protocol/compare_descriptors.hpp"
#include "protocol/compare_verdict.hpp"
#include "runtime/child_process.hpp"
#include "runtime/compare_launch.hpp"
#include "runtime/compare_process.hpp"
#include "support/bit_flags.hpp"
#include "support/error_message.hpp"
#include "support/scoped_fd.hpp"

#include <array>
#include <cerrno>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <spawn.h>

#include <sys/socket.h>
#include <sys/syslog.h>
#include <sys/types.h>

namespace {

	using CompareOperations = howdy::pam::compare_process::Operations;

	// Fixed compare environment entries that keep a threaded BLAS from reserving address space per
	// CPU thread. OpenBLAS built with OpenMP (Arch) obeys OMP_NUM_THREADS and ignores
	// OPENBLAS_NUM_THREADS; pthread builds obey OPENBLAS_NUM_THREADS.
	constexpr auto kCompareThreadPins = std::to_array<std::string_view>({
	    "OPENBLAS_NUM_THREADS=1",
	    "OMP_NUM_THREADS=1",
	});

	constexpr CompareOperations kPosixSpawnOperations = {
	    .context                   = nullptr,
	    .socketpair                = howdy::pam::runtime::internal::Socketpair,
	    .file_actions_init         = howdy::pam::runtime::internal::ActionsInit,
	    .file_actions_adddup2      = howdy::pam::runtime::internal::ActionsAdddup2,
	    .file_actions_addclosefrom = howdy::pam::runtime::internal::ActionsAddclosefrom,
	    .file_actions_destroy      = howdy::pam::runtime::internal::ActionsDestroy,
	    .spawn                     = howdy::pam::runtime::internal::Spawn,
	};

	// Moves `fd` to at least `floor`, unless it is there already. Every descriptor compare gets
	// sits above the numbers it is placed at in the child, so no dup2 there lands on another
	// source, and none is a dup2 onto itself, which would leave it close-on-exec.
	auto RaiseFd(howdy::native::ScopedFd *fd, int floor) -> bool {
		if (fd->Get() >= floor) {
			return true;
		}
		const int raised = fcntl(fd->Get(), F_DUPFD_CLOEXEC, floor);
		if (raised < 0) {
			return false;
		}
		fd->Reset(raised);
		return true;
	}

	// The private socket pair compare reports its verdict on (protocol/compare_verdict.hpp). A
	// PAM application that auto-reaps SIGCHLD, or reaps every child, takes compare's exit status
	// before Howdy can read it, so compare also reports the code as one byte here. A socket,
	// unlike a pipe, cannot be reopened through /proc/<pid>/fd, so no other process can add a byte.
	auto OpenVerdictSocket(const CompareOperations &operations, howdy::native::ScopedFd *pam_end,
	                       howdy::native::ScopedFd *compare_end) -> int {
		using howdy::native::compare_descriptors::kFirstUnusedFd;
		std::array<int, 2> socket_fds = {-1, -1};
		if (operations.socketpair(
		        operations.context, AF_UNIX,
		        howdy::native::CombineBitsAs<int>(SOCK_SEQPACKET, SOCK_CLOEXEC, SOCK_NONBLOCK), 0,
		        socket_fds.data()) != 0) {
			return errno;
		}
		pam_end->Reset(socket_fds.at(0));
		compare_end->Reset(socket_fds.at(1));
		if (!RaiseFd(pam_end, kFirstUnusedFd) || !RaiseFd(compare_end, kFirstUnusedFd)) {
			const int error_number = errno;
			pam_end->Reset();
			compare_end->Reset();
			return error_number;
		}
		return 0;
	}

	// A copy of the borrowed `fd` above every descriptor compare is given, owned by the spawn.
	auto CopyAboveCompareDescriptors(int fd, howdy::native::ScopedFd *copy) -> int {
		copy->Reset(fcntl(fd, F_DUPFD_CLOEXEC, howdy::native::compare_descriptors::kFirstUnusedFd));
		return copy->Valid() ? 0 : errno;
	}

	struct CompareChildDescriptors {
		int verdict = -1;
		int config  = -1;
		int model   = -1;
	};

	// Places the verdict socket, config and model at their fixed numbers in compare and closes
	// everything above them; all three are required.
	auto SpawnCompareProcess(const howdy::pam::CompareLaunchRequest &request, pid_t *child_pid,
	                         int *child_pidfd, const CompareChildDescriptors &descriptors,
	                         const CompareOperations &operations) -> int {
		namespace protocol = howdy::native::compare_descriptors;
		std::string           compare_path(kCompareProcessPath);
		std::string           username(request.username);
		std::array<char *, 3> args = {compare_path.data(), username.data(), nullptr};

		// Compare never calls BLAS, but a threaded BLAS linked into OpenCV starts its thread pool
		// when the library loads and reserves address space per thread (about 128 MiB per OpenMP
		// thread with Arch's OpenBLAS), which can exhaust the compare RLIMIT_AS. The pool is
		// created before main(), so the environment is the only place to limit it. These are fixed
		// values, never copied from the caller. Issues #11 and #12. Compare made no BLAS or LAPACK
		// calls when counted on OpenCV 5.0.0 and 5.1.0 (detector, align, SFace feature; 5.1 runs
		// DNN GEMM through its vendored MLAS), so the pins cost no BLAS speed. Recheck that count
		// when OpenCV changes, and revisit this if compare starts using BLAS-heavy OpenCV calls.
		// The `cv::setNumThreads` call in compare's `entrypoint.cpp` overrides the OMP_NUM_THREADS
		// pin for an OpenMP-backend OpenCV.
		auto                pins = std::ranges::to<std::vector<std::string>>(kCompareThreadPins);
		std::string         verdict_env(howdy::native::compare_verdict::kEnvironmentEntry);
		std::string         config_env(protocol::kConfigEnvironmentEntry);
		std::string         model_env(protocol::kModelEnvironmentEntry);
		std::vector<char *> environment;
		environment.reserve(pins.size() + 4);
		for (auto &pin : pins) {
			environment.push_back(pin.data());
		}
		environment.push_back(verdict_env.data());
		environment.push_back(config_env.data());
		environment.push_back(model_env.data());
		environment.push_back(nullptr);
		const std::span<char *const> compare_env(environment);

		posix_spawn_file_actions_t file_actions;
		const int init_result = operations.file_actions_init(operations.context, &file_actions);
		if (init_result != 0) {
			return init_result;
		}
		const auto add_actions = [&] -> int {
			for (const auto &[source, target] : {
			         std::pair{descriptors.verdict, howdy::native::compare_verdict::kFd},
			         std::pair{descriptors.config, protocol::kConfigFd},
			         std::pair{descriptors.model, protocol::kModelFd},
			     }) {
				if (const int result = operations.file_actions_adddup2(
				        operations.context, &file_actions, source, target);
				    result != 0) {
					return result;
				}
			}
			return operations.file_actions_addclosefrom(operations.context, &file_actions,
			                                            protocol::kFirstUnusedFd);
		};
		if (const int actions_result = add_actions(); actions_result != 0) {
			(void)operations.file_actions_destroy(operations.context, &file_actions);
			return actions_result;
		}

		const int spawn_result = operations.spawn({
		    .context     = operations.context,
		    .child_pid   = child_pid,
		    .child_pidfd = child_pidfd,
		    .path        = kCompareProcessPath,
		    .actions     = &file_actions,
		    .argv        = args,
		    .envp        = compare_env,
		});
		(void)operations.file_actions_destroy(operations.context, &file_actions);
		return spawn_result;
	}

}  // namespace

namespace howdy::pam::compare_process {

	auto ProductionOperations() -> Operations {
		return kPosixSpawnOperations;
	}

	auto Spawn(const CompareLaunchRequest &request, pid_t *child_pid, int *child_pidfd,
	           int *verdict_fd, const Operations &operations) -> int {
		if (child_pid == nullptr || child_pidfd == nullptr || verdict_fd == nullptr ||
		    request.username.empty() || request.config_fd < 0 || request.model_fd < 0) {
			return EINVAL;  // the pidfd is required, so there is nowhere to put a child without it
		}
		*child_pidfd = -1;
		*verdict_fd  = -1;
		howdy::native::ScopedFd verdict_pam_end;
		howdy::native::ScopedFd verdict_compare_end;
		if (const int error_number =
		        OpenVerdictSocket(operations, &verdict_pam_end, &verdict_compare_end);
		    error_number != 0) {
			syslog(LOG_AUTHPRIV | LOG_ERR,
			       "pam_howdy: Could not open the compare verdict socket: %s (%d)",
			       howdy::native::SystemErrorMessage(error_number).c_str(), error_number);
			return error_number;
		}
		howdy::native::ScopedFd config;
		howdy::native::ScopedFd model;
		int copy_error = CopyAboveCompareDescriptors(request.config_fd, &config);
		if (copy_error == 0) {
			copy_error = CopyAboveCompareDescriptors(request.model_fd, &model);
		}
		if (copy_error != 0) {
			syslog(LOG_AUTHPRIV | LOG_ERR,
			       "pam_howdy: Could not prepare the compare descriptors: %s (%d)",
			       howdy::native::SystemErrorMessage(copy_error).c_str(), copy_error);
			return copy_error;
		}
		int       spawn_pidfd = -1;
		const int result      = SpawnCompareProcess(request, child_pid, &spawn_pidfd,
		                                            {
		                                                .verdict = verdict_compare_end.Get(),
		                                                .config  = config.Get(),
		                                                .model   = model.Get(),
		                                            },
		                                            operations);
		// Owned at once, so it is closed on every path below that does not hand it out.
		howdy::native::ScopedFd taken_pidfd(spawn_pidfd);
		// Only compare holds these from here, so its exit ends the verdict socket.
		verdict_compare_end.Reset();
		config.Reset();
		model.Reset();
		if (result == 0 && taken_pidfd.Valid()) {
			// Taken with the child. Compare has no gone path: one the application already reaped
			// before its PID was read fails the spawn, as a pidfd_open that finds it gone does.
			if (*child_pid <= 0) {
				syslog(LOG_AUTHPRIV | LOG_ERR,
				       "pam_howdy: Compare process was reaped elsewhere before its PID was read");
				*child_pid = -1;
				return ECHILD;
			}
			*child_pidfd = taken_pidfd.Release();
			*verdict_fd  = verdict_pam_end.Release();
			return 0;
		}
		// Without one, taken right after spawn, before the PAM application can reap compare and
		// let the PID be reused.
		if (result == 0 && *child_pid > 0) {
			// A pidfd opened later could name another process, so a failure fails the spawn.
			int error_number = 0;
			*child_pidfd = howdy::pam::runtime::TakeSpawnedChildPidfd(*child_pid, &error_number);
			if (*child_pidfd < 0) {
				syslog(LOG_AUTHPRIV | LOG_ERR,
				       "pam_howdy: Could not take a pidfd for compare process: %s (%d)",
				       howdy::native::SystemErrorMessage(error_number).c_str(), error_number);
				*child_pid = -1;
				return error_number;
			}
			*verdict_fd = verdict_pam_end.Release();
		}
		return result;
	}

}  // namespace howdy::pam::compare_process
