#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace howdy::pam::runtime {

	enum class LidState : std::uint8_t {
		kUnknown,
		kOpen,
		kClosed,
	};

	enum class LidProbeStatus : std::uint8_t {
		kOk,
		kError,
	};

	struct LidStateResult {
		LidProbeStatus status = LidProbeStatus::kOk;
		LidState       state  = LidState::kUnknown;
		std::string    error_message;
	};

	auto ReadLidState() -> LidStateResult;
	auto ReadLidStateFromPattern(std::string_view pattern) -> LidStateResult;

}  // namespace howdy::pam::runtime
