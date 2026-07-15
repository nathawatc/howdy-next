#include "common/compare_sandbox.hpp"

#include "common/compare_sandbox_internal.hpp"

#include <algorithm>
#include <array>
#include <cerrno>

#include <sys/prctl.h>
#include <sys/resource.h>

namespace {

	using howdy::native::CompareSandboxResource;
	using howdy::native::CompareSandboxResult;
	using howdy::native::CompareSandboxStatus;
	using howdy::native::compare_sandbox_internal::CompareSandboxDependencies;
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

	auto finite_min(rlim_t preferred, rlim_t inherited) -> rlim_t {
		if (inherited == RLIM_INFINITY) {
			return preferred;
		}
		return std::min(preferred, inherited);
	}

	struct TimeoutLimit {
		int    seconds = 0;
		rlim_t margin  = 0;
	};

	auto timeout_limit(TimeoutLimit limit) -> rlim_t {
		const auto timeout =
		    limit.seconds > 0 ? static_cast<rlim_t>(limit.seconds) : static_cast<rlim_t>(0);
		if (timeout > kMaximumFiniteLimit - limit.margin) {
			return kMaximumFiniteLimit;
		}
		return timeout + limit.margin;
	}

	auto apply_limit(const LimitPolicy &policy, const CompareSandboxDependencies &dependencies)
	    -> CompareSandboxResult {
		rlimit inherited{};
		if (dependencies.getrlimit(dependencies.context, policy.system_resource, &inherited) != 0) {
			return {
			    .status       = CompareSandboxStatus::kLimitInspectionFailure,
			    .resource     = policy.resource,
			    .error_number = errno,
			};
		}

		const rlim_t target_hard = finite_min(policy.preferred_hard, inherited.rlim_max);
		const rlim_t target_soft =
		    finite_min(finite_min(policy.preferred_soft, inherited.rlim_cur), target_hard);
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

	auto system_getrlimit([[maybe_unused]] void *context, RlimitResource resource, rlimit *limit)
	    -> int {
		return getrlimit(resource, limit);
	}

	auto system_setrlimit([[maybe_unused]] void *context, RlimitResource resource,
	                      const rlimit *limit) -> int {
		return setrlimit(resource, limit);
	}

	auto system_prctl([[maybe_unused]] void *context, int operation, unsigned long argument2,
	                  unsigned long argument3, unsigned long argument4, unsigned long argument5)
	    -> int {
		return prctl(operation, argument2, argument3, argument4, argument5);
	}

}  // namespace

namespace howdy::native::compare_sandbox_internal {

	auto apply_compare_sandbox(int timeout_seconds, const CompareSandboxDependencies &dependencies)
	    -> CompareSandboxResult {
		if (dependencies.prctl == nullptr || dependencies.getrlimit == nullptr ||
		    dependencies.setrlimit == nullptr) {
			return {
			    .status       = CompareSandboxStatus::kNoNewPrivilegesFailure,
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

		const rlim_t minimum_cpu =
		    timeout_limit({.seconds = timeout_seconds, .margin = kCpuSoftMargin});
		const std::array<LimitPolicy, 4> policies{{
		    {
		        .system_resource = RLIMIT_CPU,
		        .resource        = CompareSandboxResource::kCpu,
		        .preferred_soft  = std::max(minimum_cpu, kCpuSoftFloor),
		        .preferred_hard =
		            std::max(timeout_limit({.seconds = timeout_seconds, .margin = kCpuHardMargin}),
		                     kCpuHardFloor),
		        .minimum_soft = minimum_cpu,
		        .minimum_hard = minimum_cpu,
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
		}};

		for (const auto &policy : policies) {
			const auto result = apply_limit(policy, dependencies);
			if (result.status != CompareSandboxStatus::kOk) {
				return result;
			}
		}
		return {};
	}

}  // namespace howdy::native::compare_sandbox_internal

namespace howdy::native {

	auto apply_compare_sandbox(int timeout_seconds) -> CompareSandboxResult {
		return compare_sandbox_internal::apply_compare_sandbox(timeout_seconds,
		                                                       {
		                                                           .context   = nullptr,
		                                                           .getrlimit = system_getrlimit,
		                                                           .setrlimit = system_setrlimit,
		                                                           .prctl     = system_prctl,
		                                                       });
	}

}  // namespace howdy::native
