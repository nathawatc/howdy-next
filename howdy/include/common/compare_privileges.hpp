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

		[[nodiscard]] auto ok() const noexcept -> bool {
			return status == ComparePrivilegeStatus::kOk;
		}
	};

	[[nodiscard]] auto drop_compare_privileges() -> ComparePrivilegeResult;

}  // namespace howdy::native
