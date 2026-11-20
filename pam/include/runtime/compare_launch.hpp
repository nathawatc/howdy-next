#pragma once

#include <string>

namespace howdy::pam {
	using CompareCancellationRequestedFn = bool (*)(void *context);

	// Compare runs in descriptor mode (protocol/compare_descriptors.hpp). The descriptors are
	// borrowed from the runtime session, which keeps them open until the authentication ends.
	struct CompareLaunchRequest {
		std::string username;
		int         config_fd = -1;
		int         model_fd  = -1;
	};

}  // namespace howdy::pam
