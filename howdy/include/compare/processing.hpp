#pragma once

#include "compare/capture_session.hpp"
#include "compare/privileges.hpp"
#include "protocol/compare_exit.hpp"

#include <variant>

namespace howdy::native::compare_processing_internal {

	struct CompareProcessingInvalidDependencies {};

	using CompareProcessingResult =
	    std::variant<CompareProcessingInvalidDependencies, CompareCaptureOpenResult,
	                 ComparePrivilegeResult, CompareExit>;

	using OpenCaptureFn     = CompareCaptureOpenResult (*)(void *context);
	using DropPrivilegesFn  = ComparePrivilegeResult (*)(void *context);
	using ConstructEngineFn = void (*)(void *context);
	using ResetTimeoutFn    = void (*)(void *context);
	using RunFrameLoopFn    = CompareExit (*)(void *context);

	struct CompareProcessingDependencies {
		void             *context          = nullptr;
		OpenCaptureFn     open_capture     = nullptr;
		DropPrivilegesFn  drop_privileges  = nullptr;
		ConstructEngineFn construct_engine = nullptr;
		ResetTimeoutFn    reset_timeout    = nullptr;
		RunFrameLoopFn    run_frame_loop   = nullptr;
	};

	[[nodiscard]] auto run_compare_processing(const CompareProcessingDependencies &dependencies)
	    -> CompareProcessingResult;

}  // namespace howdy::native::compare_processing_internal
