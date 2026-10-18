#pragma once

#include <cstdint>

namespace howdy::native {

	enum class CompareSandboxStatus : std::uint8_t {
		kOk,
		kNoNewPrivilegesFailure,
		kLimitInspectionFailure,
		kLimitBelowMinimum,
		kLimitApplicationFailure,
	};

	enum class CompareSandboxResource : std::uint8_t {
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

	enum class CompareSeccompStatus : std::uint8_t {
		kOk,
		kUnsupported,
		kFilterConstructionFailure,
		kInstallationFailure,
	};

	struct CompareSeccompResult {
		CompareSeccompStatus status       = CompareSeccompStatus::kOk;
		int                  error_number = 0;
	};

	[[nodiscard]] auto ApplyCompareSandbox(int timeout_seconds) -> CompareSandboxResult;

	[[nodiscard]] auto ApplyCompareSeccompFilter() -> CompareSeccompResult;

}  // namespace howdy::native
