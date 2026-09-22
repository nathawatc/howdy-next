#include "compare/processing.hpp"

#include "compare/capture_session.hpp"
#include "compare/privileges.hpp"
#include "protocol/compare_exit.hpp"

#include <exception>
#include <thread>

namespace howdy::native::compare_processing_internal {

	auto RunCompareProcessing(const CompareProcessingDependencies &dependencies)
	    -> CompareProcessingResult {
		if (dependencies.initialize_model == nullptr || dependencies.open_capture == nullptr ||
		    dependencies.drop_privileges == nullptr || dependencies.construct_engine == nullptr ||
		    dependencies.reset_timeout == nullptr || dependencies.run_frame_loop == nullptr) {
			return CompareExit::kAbort;
		}

		bool               model_ready = false;
		std::exception_ptr model_error;
		std::jthread       model_worker([&]() -> void {
			try {
				model_ready = dependencies.initialize_model(dependencies.context);
			} catch (...) {
				model_error = std::current_exception();
			}
		});

		CompareCaptureOpenResult capture_open;
		std::exception_ptr       capture_error;
		try {
			capture_open = dependencies.open_capture(dependencies.context);
		} catch (...) {
			capture_error = std::current_exception();
		}
		// No model worker may survive privilege dropping, including on startup failure.
		model_worker.join();
		if (model_error) {
			std::rethrow_exception(model_error);
		}
		if (!model_ready) {
			return CompareExit::kAbort;
		}
		if (capture_error) {
			std::rethrow_exception(capture_error);
		}
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
