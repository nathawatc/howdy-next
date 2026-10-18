#pragma once

#include "compare/capture_session.hpp"
#include "compare/privileges.hpp"
#include "compare/sandbox.hpp"
#include "vision/face_inference.hpp"

namespace howdy::native {

	// Test seam for capture, inference, and the privilege transition.
	struct CompareEntrypointDependencies {
		CompareCaptureDependencies capture;
		FaceInferenceOperations    inference;
		void                      *context                = nullptr;
		ComparePrivilegeResult (*drop_privileges)(void *) = nullptr;
		bool (*warm_model)(void *)                        = nullptr;
		CompareSeccompResult (*apply_seccomp)(void *)     = nullptr;
	};

}  // namespace howdy::native
