#pragma once

#include "prompt/workaround.hpp"

namespace howdy::pam {

	struct PamModuleArguments {
		int                flags = 0;
		int                argc  = 0;
		const char *const *argv  = nullptr;
	};

	struct PamOptions {
		Workaround workaround = Workaround::kOff;
	};

	__attribute__((visibility("hidden"))) auto parse_pam_options(PamModuleArguments arguments)
	    -> PamOptions;

}  // namespace howdy::pam
