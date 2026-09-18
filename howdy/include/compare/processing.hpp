#pragma once

#include "compare/capture_session.hpp"
#include "compare/privileges.hpp"
#include "protocol/compare_exit.hpp"

#include <variant>

namespace howdy::native::compare_processing_internal {

	using CompareProcessingResult =
	    std::variant<CompareCaptureOpenResult, ComparePrivilegeResult, CompareExit>;

	using OpenCaptureFn     = CompareCaptureOpenResult(void *context);
	using DropPrivilegesFn  = ComparePrivilegeResult(void *context);
	using ConstructEngineFn = void(void *context);
	using ResetTimeoutFn    = void(void *context);
	using RunFrameLoopFn    = CompareExit(void *context);

	struct CompareProcessingDependencies {
		void              *context = nullptr;
		OpenCaptureFn     &open_capture;
		DropPrivilegesFn  &drop_privileges;
		ConstructEngineFn &construct_engine;
		ResetTimeoutFn    &reset_timeout;
		RunFrameLoopFn    &run_frame_loop;
	};

	[[nodiscard]] auto RunCompareProcessing(const CompareProcessingDependencies &dependencies)
	    -> CompareProcessingResult;

}  // namespace howdy::native::compare_processing_internal
