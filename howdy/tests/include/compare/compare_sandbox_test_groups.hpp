#pragma once

#include <cstdint>
#include <string_view>

namespace howdy::test::compare_sandbox {

	inline constexpr std::uint32_t kTcgets     = 0x5401U;
	inline constexpr std::uint32_t kTcgets2    = 0x802C542AU;
	inline constexpr std::uint32_t kTiocgwinsz = 0x5413U;

}  // namespace howdy::test::compare_sandbox

auto RunCompareSandboxLimitTests() -> bool;
auto RunCompareSandboxSeccompTests() -> bool;
auto RunCompareSandboxSeccompExecTests() -> bool;
auto RunCompareSandboxSeccompProbe(std::string_view action) -> int;
