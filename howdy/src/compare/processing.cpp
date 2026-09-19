#include "compare/processing.hpp"

#include "compare/capture_session.hpp"
#include "compare/privileges.hpp"
#include "protocol/compare_exit.hpp"

namespace howdy::native::compare_processing_internal {

	auto RunCompareProcessing(const CompareProcessingDependencies &dependencies)
	    -> CompareProcessingResult {
		if (dependencies.open_capture == nullptr || dependencies.drop_privileges == nullptr ||
		    dependencies.construct_engine == nullptr || dependencies.reset_timeout == nullptr ||
		    dependencies.run_frame_loop == nullptr) {
			return CompareExit::kAbort;
		}

		auto capture_open = dependencies.open_capture(dependencies.context);
		if (capture_open.status != CompareCaptureOpenStatus::kOk) {
			return capture_open;
		}

		auto privilege_result = dependencies.drop_privileges(dependencies.context);
		if (!ComparePrivilegeSucceeded(privilege_result)) {
			return privilege_result;
		}

		dependencies.construct_engine(dependencies.context);
		dependencies.reset_timeout(dependencies.context);
		return dependencies.run_frame_loop(dependencies.context);
	}

}  // namespace howdy::native::compare_processing_internal
