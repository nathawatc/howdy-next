#pragma once

namespace howdy::native {

	template <typename ExitCode>
	[[nodiscard]] constexpr auto EncodeWaitExitStatus(ExitCode exit_code) -> int {
		return static_cast<int>(static_cast<unsigned int>(exit_code) << 8U);
	}

	// wait(2)-style status of a process terminated by `signal`.
	[[nodiscard]] constexpr auto EncodeWaitSignalStatus(int signal, bool core_dumped) -> int {
		constexpr unsigned int term_signal_mask = 0x7fU;
		constexpr unsigned int core_dump_bit    = 0x80U;
		return static_cast<int>((static_cast<unsigned int>(signal) & term_signal_mask) |
		                        (core_dumped ? core_dump_bit : 0U));
	}

}  // namespace howdy::native
