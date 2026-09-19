#pragma once

namespace howdy::native {

	template <typename ExitCode>
	[[nodiscard]] constexpr auto EncodeWaitExitStatus(ExitCode exit_code) -> int {
		return static_cast<int>(static_cast<unsigned int>(exit_code) << 8U);
	}

}  // namespace howdy::native
