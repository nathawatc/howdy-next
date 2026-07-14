#pragma once

namespace howdy::native {

	enum class CompareSandboxStatus {
		kOk,
		kNoNewPrivilegesFailure,
		kLimitInspectionFailure,
		kLimitBelowMinimum,
		kLimitApplicationFailure,
	};

	enum class CompareSandboxResource {
		kNone,
		kCpu,
		kOpenFiles,
		kCore,
		kAddressSpace,
	};

	struct CompareSandboxResult {
		CompareSandboxStatus   status       = CompareSandboxStatus::kOk;
		CompareSandboxResource resource     = CompareSandboxResource::kNone;
		int                    error_number = 0;
	};

	[[nodiscard]] auto apply_compare_sandbox(int timeout_seconds) -> CompareSandboxResult;

}  // namespace howdy::native
