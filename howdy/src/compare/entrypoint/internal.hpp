#pragma once

#include "compare/sandbox.hpp"
#include "support/scoped_fd.hpp"

#include <cstdint>
#include <optional>
#include <string>

#include <sys/types.h>

namespace howdy::native::compare_entrypoint_private {

	// The verdict socket as found at startup, so the report can check that the descriptor still
	// names it and never writes into a file that took the number meanwhile.
	struct VerdictSocket {
		bool  announced = false;
		dev_t device    = 0;
		ino_t inode     = 0;
	};

	// The config and model descriptors PAM passed (protocol/compare_descriptors.hpp). They are
	// the only source compare reads them from; anything missing or unusable fails closed.
	struct RuntimeDescriptors {
		enum class Mode : std::uint8_t {
			kReady,
			kNotAnnounced,
			kNotOpen,
			kNoVerdict,
		};

		Mode                    mode = Mode::kNotAnnounced;
		howdy::native::ScopedFd config;
		howdy::native::ScopedFd model;
	};

	[[nodiscard]] auto FindVerdictSocket() noexcept -> VerdictSocket;
	auto               ReportVerdict(const VerdictSocket &socket, int exit_code) noexcept -> int;
	[[nodiscard]] auto TakeRuntimeDescriptors(bool verdict_announced) noexcept
	    -> RuntimeDescriptors;
	[[nodiscard]] auto RuntimeDescriptorsError(const RuntimeDescriptors &descriptors)
	    -> std::optional<std::string>;

	void ReportSandboxFailure(const howdy::native::CompareSandboxResult &result);
	void ReportSeccompFailure(const howdy::native::CompareSeccompResult &result);

}  // namespace howdy::native::compare_entrypoint_private
