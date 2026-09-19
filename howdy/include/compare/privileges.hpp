#pragma once

#include <cstdint>
#include <string>

namespace howdy::native {

	enum class ComparePrivilegeStatus : std::uint8_t {
		kOk,
		kLookupFailed,
		kInvalidIdentity,
		kCapabilityFailure,
		kVerificationFailure,
	};

	struct ComparePrivilegeResult {
		ComparePrivilegeStatus status = ComparePrivilegeStatus::kVerificationFailure;
		std::string            error_message;
	};

	[[nodiscard]] constexpr auto
	ComparePrivilegeSucceeded(const ComparePrivilegeResult &result) noexcept -> bool {
		return result.status == ComparePrivilegeStatus::kOk;
	}

	[[nodiscard]] auto DropComparePrivileges() -> ComparePrivilegeResult;

}  // namespace howdy::native
