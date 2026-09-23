#pragma once

#include "compare/capture_session.hpp"
#include "compare/privileges.hpp"
#include "protocol/compare_exit.hpp"

#include <functional>
#include <variant>

namespace howdy::native::compare_processing_internal {

	using CompareProcessingResult =
	    std::variant<CompareCaptureOpenResult, ComparePrivilegeResult, CompareExit>;

	using InitializeModelFn = bool (*)(void *context);
	using OpenCaptureFn     = CompareCaptureOpenResult (*)(void *context);
	using DropPrivilegesFn  = ComparePrivilegeResult (*)(void *context);
	using ConstructEngineFn = void (*)(void *context);
	using ResetTimeoutFn    = void (*)(void *context);
	using WarmModelFn       = bool (*)(void *context);
	using RunFrameLoopFn    = CompareExit (*)(void                  *context,
	                                          std::function<void()> &before_first_encode);

	struct CompareProcessingDependencies {
		void             *context          = nullptr;
		InitializeModelFn initialize_model = nullptr;
		OpenCaptureFn     open_capture     = nullptr;
		DropPrivilegesFn  drop_privileges  = nullptr;
		ConstructEngineFn construct_engine = nullptr;
		ResetTimeoutFn    reset_timeout    = nullptr;
		WarmModelFn       warm_model       = nullptr;
		RunFrameLoopFn    run_frame_loop   = nullptr;
	};

	[[nodiscard]] auto RunCompareProcessing(const CompareProcessingDependencies &dependencies)
	    -> CompareProcessingResult;

}  // namespace howdy::native::compare_processing_internal
