#include "compare/processing.hpp"

namespace howdy::native::compare_processing_internal {

	auto RunCompareProcessing(const CompareProcessingDependencies &dependencies)
	    -> CompareProcessingResult {
		if (dependencies.open_capture == nullptr || dependencies.drop_privileges == nullptr ||
		    dependencies.construct_engine == nullptr || dependencies.reset_timeout == nullptr ||
		    dependencies.run_frame_loop == nullptr) {
			return CompareProcessingInvalidDependencies{};
		}

		auto capture_open = dependencies.open_capture(dependencies.context);
		if (capture_open.status != CompareCaptureOpenStatus::kOk) {
			return capture_open;
		}

		auto privilege_result = dependencies.drop_privileges(dependencies.context);
		if (!privilege_result.Ok()) {
			return privilege_result;
		}

		dependencies.construct_engine(dependencies.context);
		dependencies.reset_timeout(dependencies.context);
		return dependencies.run_frame_loop(dependencies.context);
	}

}  // namespace howdy::native::compare_processing_internal
