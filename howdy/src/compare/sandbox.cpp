#include "compare/sandbox.hpp"

#include "compare/sandbox/internal.hpp"
#include "support/eintr.hpp"
#include "support/prctl_types.hpp"
#include "support/scoped_fd.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <sys/resource.h>
#include <sys/types.h>

#include <linux/prctl.h>

namespace {

	using howdy::native::CompareSandboxResource;
	using howdy::native::CompareSandboxResult;
	using howdy::native::CompareSandboxStatus;
	using howdy::native::compare_sandbox_internal::CompareSandboxDependencies;
	using howdy::native::compare_sandbox_internal::CompareTracingDependencies;
	using howdy::native::compare_sandbox_internal::RlimitResource;

	constexpr rlim_t kPreferredOpenFiles = 32;
	// Measured peak is five descriptors; 16 leaves room beyond stdio, camera, config/model files,
	// and OpenCV's internal descriptors.
	constexpr rlim_t kMinimumOpenFiles = 16;
	constexpr rlim_t kPreferredAddressSpace =
	    static_cast<rlim_t>(5ULL * 1024ULL * 1024ULL * 1024ULL / 2ULL);
	// Measured VmPeak is 1.672 GiB. Keep a conservative 2 GiB policy floor.
	constexpr rlim_t kMinimumAddressSpace = static_cast<rlim_t>(2ULL * 1024ULL * 1024ULL * 1024ULL);
	constexpr rlim_t kCpuSoftMargin       = 5;
	constexpr rlim_t kCpuHardMargin       = 10;
	constexpr rlim_t kCpuSoftFloor        = 15;
	constexpr rlim_t kCpuHardFloor        = 20;
	constexpr rlim_t kMaximumFiniteLimit  = RLIM_INFINITY - static_cast<rlim_t>(1);

	struct LimitPolicy {
		RlimitResource         system_resource;
		CompareSandboxResource resource;
		rlim_t                 preferred_soft;
		rlim_t                 preferred_hard;
		rlim_t                 minimum_soft;
		rlim_t                 minimum_hard;
	};

	auto FiniteMin(rlim_t preferred, rlim_t inherited) -> rlim_t {
		if (inherited == RLIM_INFINITY) {
			return preferred;
		}
		return std::min(preferred, inherited);
	}

	struct TimeoutLimit {
		int    seconds = 0;
		rlim_t margin  = 0;
	};

	auto ComputeTimeoutLimit(TimeoutLimit limit) -> rlim_t {
		const auto timeout =
		    limit.seconds > 0 ? static_cast<rlim_t>(limit.seconds) : static_cast<rlim_t>(0);
		if (timeout > kMaximumFiniteLimit - limit.margin) {
			return kMaximumFiniteLimit;
		}
		return timeout + limit.margin;
	}

	auto ApplyLimit(const LimitPolicy &policy, const CompareSandboxDependencies &dependencies)
	    -> CompareSandboxResult {
		rlimit inherited{};
		if (dependencies.getrlimit(dependencies.context, policy.system_resource, &inherited) != 0) {
			return {
			    .status       = CompareSandboxStatus::kLimitInspectionFailure,
			    .resource     = policy.resource,
			    .error_number = errno,
			};
		}

		const rlim_t target_hard = FiniteMin(policy.preferred_hard, inherited.rlim_max);
		const rlim_t target_soft =
		    FiniteMin(FiniteMin(policy.preferred_soft, inherited.rlim_cur), target_hard);
		if (target_soft < policy.minimum_soft || target_hard < policy.minimum_hard) {
			return {
			    .status       = CompareSandboxStatus::kLimitBelowMinimum,
			    .resource     = policy.resource,
			    .error_number = 0,
			};
		}

		const rlimit target{
		    .rlim_cur = target_soft,
		    .rlim_max = target_hard,
		};
		if (dependencies.setrlimit(dependencies.context, policy.system_resource, &target) != 0) {
			return {
			    .status       = CompareSandboxStatus::kLimitApplicationFailure,
			    .resource     = policy.resource,
			    .error_number = errno,
			};
		}
		return {};
	}

	auto SystemGetrlimit([[maybe_unused]] void *context, RlimitResource resource, rlimit *limit)
	    -> int {
		return getrlimit(resource, limit);
	}

	auto SystemSetrlimit([[maybe_unused]] void *context, RlimitResource resource,
	                     const rlimit *limit) -> int {
		return setrlimit(resource, limit);
	}

	// A task status file is about 1.5 KiB; anything far larger is not the file we expect.
	constexpr std::size_t kMaximumStatusSize = 64UZ * 1024UZ;

	struct DirectoryCloser {
		void operator()(DIR *directory) const noexcept {
			(void)closedir(directory);
		}
	};

	auto ReadStatusFile(int task_directory, const std::string &path, std::string *status) -> int {
		const howdy::native::ScopedFd fd(howdy::native::RetryOnEintr([&] -> int {
			return openat(task_directory, path.c_str(), O_RDONLY | O_CLOEXEC | O_NOCTTY);
		}));
		if (fd.Get() < 0) {
			return -1;
		}
		std::array<char, 4096> buffer{};
		while (true) {
			const ssize_t count = howdy::native::RetryOnEintr([&] -> ssize_t {
				return read(fd.Get(), buffer.data(), buffer.size());
			});
			if (count < 0) {
				return -1;
			}
			if (count == 0) {
				return 0;
			}
			status->append(buffer.data(), static_cast<std::size_t>(count));
			if (status->size() > kMaximumStatusSize) {
				errno = EFBIG;
				return -1;
			}
		}
	}

	// ptrace attaches to a single thread, and /proc/self/status reports only the main thread's
	// tracer, so a thread started before main could be traced unseen. Check every task.
	auto SystemFindTracerPid([[maybe_unused]] void *context, pid_t *tracer_pid) -> int {
		const int task_fd = howdy::native::RetryOnEintr([] -> int {
			return open("/proc/self/task", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		});
		if (task_fd < 0) {
			return -1;
		}
		const std::unique_ptr<DIR, DirectoryCloser> directory(fdopendir(task_fd));
		if (directory == nullptr) {
			const int error_number = errno;
			(void)close(task_fd);
			errno = error_number;
			return -1;
		}

		std::size_t checked = 0;
		while (true) {
			errno                     = 0;
			const dirent *const entry = readdir(directory.get());
			if (entry == nullptr) {
				if (errno != 0) {
					return -1;
				}
				break;
			}
			const std::string_view name(static_cast<const char *>(entry->d_name));
			if (name.empty() || name.find_first_not_of("0123456789") != std::string_view::npos) {
				continue;
			}
			std::string status;
			if (ReadStatusFile(dirfd(directory.get()), std::string(name) + "/status", &status) !=
			    0) {
				// A thread that exited after readdir can no longer be traced.
				if (errno == ENOENT || errno == ESRCH) {
					continue;
				}
				return -1;
			}
			pid_t task_tracer = 0;
			if (!howdy::native::compare_sandbox_internal::ParseTracerPid(status, &task_tracer)) {
				errno = EINVAL;
				return -1;
			}
			++checked;
			if (task_tracer != 0) {
				*tracer_pid = task_tracer;
				return 0;
			}
		}
		// The calling thread is always listed; an empty walk means this is not the task list.
		if (checked == 0) {
			errno = EINVAL;
			return -1;
		}
		*tracer_pid = 0;
		return 0;
	}

}  // namespace

namespace howdy::native::compare_sandbox_internal {

	auto ParseTracerPid(std::string_view status, pid_t *tracer_pid) -> bool {
		constexpr std::string_view field_name = "TracerPid:";
		std::size_t                line       = 0;
		while (line < status.size()) {
			const std::size_t end  = std::min(status.find('\n', line), status.size());
			auto              text = status.substr(line, end - line);
			line                   = end + 1;
			if (!text.starts_with(field_name)) {
				continue;
			}
			text.remove_prefix(field_name.size());
			const auto first = text.find_first_not_of(" \t");
			if (first == std::string_view::npos) {
				return false;
			}
			text.remove_prefix(first);
			const char *const digits_end = std::to_address(text.end());
			pid_t             value      = 0;
			const auto parsed = std::from_chars(std::to_address(text.begin()), digits_end, value);
			if (parsed.ec != std::errc{} || parsed.ptr != digits_end || value < 0) {
				return false;
			}
			*tracer_pid = value;
			return true;
		}
		return false;
	}

	auto DisableCompareTracing(const CompareTracingDependencies &dependencies)
	    -> CompareSandboxResult {
		if (dependencies.prctl == nullptr || dependencies.find_tracer_pid == nullptr) {
			return {
			    .status       = CompareSandboxStatus::kInvalidDependencies,
			    .resource     = CompareSandboxResource::kNone,
			    .error_number = EINVAL,
			};
		}

		// Non-root callers keep their UID, so without this a same-UID process could ptrace
		// compare or collect a pipe core_pattern dump while it holds the model descriptor.
		// DropComparePrivileges clears it again because a UID change resets it.
		if (dependencies.prctl(dependencies.context, PR_SET_DUMPABLE, 0, 0, 0, 0) != 0) {
			return {
			    .status       = CompareSandboxStatus::kDumpableFailure,
			    .resource     = CompareSandboxResource::kNone,
			    .error_number = errno,
			};
		}

		// ptrace checks permission only at attach time, so a tracer that attached between exec
		// and the prctl above stays attached. Check after the prctl so none can attach later.
		// Best effort only: compare is dumpable from exec until the prctl, so a same-UID process
		// can read memory or take descriptors (pidfd_getfd) and detach before this runs, and a
		// tracer outside our PID namespace reads as TracerPid 0. Closing that window needs the
		// caller to withhold the descriptors until compare is non-dumpable.
		pid_t tracer_pid = -1;
		if (dependencies.find_tracer_pid(dependencies.context, &tracer_pid) != 0) {
			return {
			    .status       = CompareSandboxStatus::kTracerInspectionFailure,
			    .resource     = CompareSandboxResource::kNone,
			    .error_number = errno,
			};
		}
		if (tracer_pid != 0) {
			return {
			    .status       = CompareSandboxStatus::kTracerAttached,
			    .resource     = CompareSandboxResource::kNone,
			    .error_number = 0,
			};
		}
		return {};
	}

	auto ApplyCompareSandbox(int timeout_seconds, const CompareSandboxDependencies &dependencies)
	    -> CompareSandboxResult {
		if (dependencies.prctl == nullptr || dependencies.getrlimit == nullptr ||
		    dependencies.setrlimit == nullptr) {
			return {
			    .status       = CompareSandboxStatus::kInvalidDependencies,
			    .resource     = CompareSandboxResource::kNone,
			    .error_number = EINVAL,
			};
		}

		if (dependencies.prctl(dependencies.context, PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
			return {
			    .status       = CompareSandboxStatus::kNoNewPrivilegesFailure,
			    .resource     = CompareSandboxResource::kNone,
			    .error_number = errno,
			};
		}

		const rlim_t preferred_cpu_soft =
		    std::max(ComputeTimeoutLimit({.seconds = timeout_seconds, .margin = kCpuSoftMargin}),
			         kCpuSoftFloor);
		const std::array<LimitPolicy, 4> policies{
		    {
		        {
		            .system_resource = RLIMIT_CPU,
		            .resource        = CompareSandboxResource::kCpu,
		            .preferred_soft  = preferred_cpu_soft,
		            .preferred_hard  = std::max(
		                ComputeTimeoutLimit({.seconds = timeout_seconds, .margin = kCpuHardMargin}),
		                kCpuHardFloor),
		            .minimum_soft = 0,
		            .minimum_hard = 0,
		        },
		        {
		            .system_resource = RLIMIT_NOFILE,
		            .resource        = CompareSandboxResource::kOpenFiles,
		            .preferred_soft  = kPreferredOpenFiles,
		            .preferred_hard  = kPreferredOpenFiles,
		            .minimum_soft    = kMinimumOpenFiles,
		            .minimum_hard    = kMinimumOpenFiles,
		        },
		        {
		            .system_resource = RLIMIT_CORE,
		            .resource        = CompareSandboxResource::kCore,
		            .preferred_soft  = 0,
		            .preferred_hard  = 0,
		            .minimum_soft    = 0,
		            .minimum_hard    = 0,
		        },
		        {
		            .system_resource = RLIMIT_AS,
		            .resource        = CompareSandboxResource::kAddressSpace,
		            .preferred_soft  = kPreferredAddressSpace,
		            .preferred_hard  = kPreferredAddressSpace,
		            .minimum_soft    = kMinimumAddressSpace,
		            .minimum_hard    = kMinimumAddressSpace,
		        },
		    },
		};

		for (const auto &policy : policies) {
			const auto result = ApplyLimit(policy, dependencies);
			if (result.status != CompareSandboxStatus::kOk) {
				return result;
			}
		}
		return {};
	}

}  // namespace howdy::native::compare_sandbox_internal

namespace howdy::native {

	auto DisableCompareTracing() -> CompareSandboxResult {
		return compare_sandbox_internal::DisableCompareTracing({
		    .context         = nullptr,
		    .prctl           = howdy::native::SystemPrctl,
		    .find_tracer_pid = SystemFindTracerPid,
		});
	}

	auto ApplyCompareSandbox(int timeout_seconds) -> CompareSandboxResult {
		return compare_sandbox_internal::ApplyCompareSandbox(
		    timeout_seconds, {
		                         .context   = nullptr,
		                         .getrlimit = SystemGetrlimit,
		                         .setrlimit = SystemSetrlimit,
		                         .prctl     = howdy::native::SystemPrctl,
		                     });
	}

}  // namespace howdy::native
