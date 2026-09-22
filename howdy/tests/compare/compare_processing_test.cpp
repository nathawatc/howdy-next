#include "compare/capture_session.hpp"
#include "compare/privileges.hpp"
#include "compare/processing.hpp"
#include "protocol/compare_exit.hpp"
#include "test_support.hpp"

#include <array>
#include <chrono>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

namespace {

	using howdy::test::Expect;

	using howdy::native::CompareCaptureOpenStatus;
	using howdy::native::CompareExit;
	using howdy::native::ComparePrivilegeStatus;
	using howdy::native::compare_processing_internal::CompareProcessingDependencies;
	using howdy::native::compare_processing_internal::CompareProcessingResult;

	struct FakeContext {
		CompareCaptureOpenStatus open_status      = CompareCaptureOpenStatus::kOk;
		ComparePrivilegeStatus   privilege_status = ComparePrivilegeStatus::kOk;
		CompareExit              processing_exit  = CompareExit::kSuccess;
		std::string              privilege_error  = "non-root process has capabilities";
		std::vector<std::string> events;
		std::binary_semaphore    model_started{0};
		std::binary_semaphore    camera_opened{0};
		std::thread::id          main_thread        = std::this_thread::get_id();
		bool                     model_ready        = true;
		bool                     model_throws       = false;
		bool                     camera_throws      = false;
		bool                     model_finished     = false;
		bool                     model_on_worker    = false;
		bool                     camera_on_main     = false;
		bool                     startup_overlapped = false;
		bool                     joined_before_drop = false;
	};

	auto InitializeModel(void *raw_context) -> bool {
		auto &context           = *static_cast<FakeContext *>(raw_context);
		context.model_on_worker = std::this_thread::get_id() != context.main_thread;
		context.model_started.release();
		context.camera_opened.acquire();
		context.model_finished = true;
		if (context.model_throws) {
			throw std::runtime_error("model failed");
		}
		return context.model_ready;
	}

	auto OpenCapture(void *raw_context) -> howdy::native::CompareCaptureOpenResult {
		auto &context = *static_cast<FakeContext *>(raw_context);
		context.events.emplace_back("open");
		context.camera_on_main     = std::this_thread::get_id() == context.main_thread;
		context.startup_overlapped = context.model_started.try_acquire_for(std::chrono::seconds(2));
		context.camera_opened.release();
		if (context.camera_throws) {
			throw std::runtime_error("camera threw");
		}
		return {
		    .status = context.open_status,
		    .error_message =
		        context.open_status == CompareCaptureOpenStatus::kOpenFailed ? "camera failed" : "",
		};
	}

	auto DropPrivileges(void *raw_context) -> howdy::native::ComparePrivilegeResult {
		auto &context = *static_cast<FakeContext *>(raw_context);
		context.events.emplace_back("drop");
		context.joined_before_drop = context.model_finished;
		return {
		    .status        = context.privilege_status,
		    .error_message = context.privilege_status == ComparePrivilegeStatus::kOk
		                         ? ""
		                         : context.privilege_error,
		};
	}

	void ConstructEngine(void *raw_context) {
		auto &context = *static_cast<FakeContext *>(raw_context);
		context.events.emplace_back("engine");
	}

	void ResetTimeout(void *raw_context) {
		auto &context = *static_cast<FakeContext *>(raw_context);
		context.events.emplace_back("reset");
	}

	auto RunFrameLoop(void *raw_context) -> CompareExit {
		auto &context = *static_cast<FakeContext *>(raw_context);
		context.events.emplace_back("first next_frame");
		return context.processing_exit;
	}

	auto DependenciesFor(FakeContext &context) -> CompareProcessingDependencies {
		return {
		    .context          = &context,
		    .initialize_model = InitializeModel,
		    .open_capture     = OpenCapture,
		    .drop_privileges  = DropPrivileges,
		    .construct_engine = ConstructEngine,
		    .reset_timeout    = ResetTimeout,
		    .run_frame_loop   = RunFrameLoop,
		};
	}

	auto Run(FakeContext &context) -> CompareProcessingResult {
		return howdy::native::compare_processing_internal::RunCompareProcessing(
		    DependenciesFor(context));
	}

	auto TestMissingDependenciesFailClosed() -> bool {
		FakeContext context;
		bool        ok = true;

		const auto expect_abort = [&](CompareProcessingDependencies dependencies,
		                              std::string_view              label) -> void {
			context.events.clear();
			const auto result =
			    howdy::native::compare_processing_internal::RunCompareProcessing(dependencies);
			const auto *processing_exit = std::get_if<CompareExit>(&result);
			ok &= Expect(processing_exit != nullptr && *processing_exit == CompareExit::kAbort,
			             std::string(label) + " fails closed");
			ok &=
			    Expect(context.events.empty(), std::string(label) + " invokes no processing stage");
		};

		auto dependencies             = DependenciesFor(context);
		dependencies.initialize_model = nullptr;
		expect_abort(dependencies, "missing model initialization");
		dependencies              = DependenciesFor(context);
		dependencies.open_capture = nullptr;
		expect_abort(dependencies, "missing open capture");
		dependencies                 = DependenciesFor(context);
		dependencies.drop_privileges = nullptr;
		expect_abort(dependencies, "missing privilege drop");
		dependencies                  = DependenciesFor(context);
		dependencies.construct_engine = nullptr;
		expect_abort(dependencies, "missing engine construction");
		dependencies               = DependenciesFor(context);
		dependencies.reset_timeout = nullptr;
		expect_abort(dependencies, "missing timeout reset");
		dependencies                = DependenciesFor(context);
		dependencies.run_frame_loop = nullptr;
		expect_abort(dependencies, "missing frame loop");
		return ok;
	}

	auto TestSuccessfulPrivilegeValidationReachesProcessing() -> bool {
		FakeContext context;
		const auto  result = Run(context);

		bool        ok              = true;
		const auto *processing_exit = std::get_if<CompareExit>(&result);
		ok &= Expect(processing_exit != nullptr, "successful validation returns frame-loop result");
		if (processing_exit != nullptr) {
			ok &= Expect(*processing_exit == CompareExit::kSuccess,
			             "successful frame-loop result propagates");
		}
		ok &= Expect(context.events == std::vector<std::string>{"open", "drop", "engine", "reset",
		                                                        "first next_frame"},
		             "successful privilege validation reaches every processing stage");
		ok &=
		    Expect(context.startup_overlapped && context.model_on_worker && context.camera_on_main,
		           "model initialization overlaps camera setup on the main thread");
		ok &= Expect(context.joined_before_drop, "model worker finishes before privilege drop");
		return ok;
	}

	auto TestOpenFailure() -> bool {
		FakeContext context;
		context.open_status = CompareCaptureOpenStatus::kOpenFailed;
		const auto result   = Run(context);

		bool        ok             = true;
		const auto *capture_result = std::get_if<howdy::native::CompareCaptureOpenResult>(&result);
		ok &= Expect(capture_result != nullptr, "open failure returns capture result");
		if (capture_result != nullptr) {
			ok &= Expect(capture_result->status == CompareCaptureOpenStatus::kOpenFailed,
			             "open failure status is preserved");
			ok &=
			    Expect(capture_result->error_message == "camera failed", "open error is preserved");
		}
		ok &= Expect(context.events == std::vector<std::string>{"open"},
		             "open failure skips all later stages");
		ok &= Expect(context.model_finished, "camera failure joins the model worker before return");
		return ok;
	}

	auto TestInvalidCameraDependencies() -> bool {
		FakeContext context;
		context.open_status = CompareCaptureOpenStatus::kInvalidDependencies;
		const auto result   = Run(context);

		bool        ok             = true;
		const auto *capture_result = std::get_if<howdy::native::CompareCaptureOpenResult>(&result);
		ok &=
		    Expect(capture_result != nullptr, "invalid camera dependencies return capture result");
		if (capture_result != nullptr) {
			ok &= Expect(capture_result->status == CompareCaptureOpenStatus::kInvalidDependencies,
			             "invalid camera dependency status is preserved");
		}
		ok &= Expect(context.events == std::vector<std::string>{"open"},
		             "invalid camera dependencies skip all later stages");
		return ok;
	}

	auto TestNonRootCapabilityFailureStopsProcessing() -> bool {
		FakeContext context;
		context.privilege_status = ComparePrivilegeStatus::kVerificationFailure;
		const auto result        = Run(context);

		bool        ok               = true;
		const auto *privilege_result = std::get_if<howdy::native::ComparePrivilegeResult>(&result);
		ok &= Expect(privilege_result != nullptr, "capability failure returns privilege result");
		if (privilege_result != nullptr) {
			ok &= Expect(privilege_result->status == ComparePrivilegeStatus::kVerificationFailure,
			             "capability failure status is preserved");
			ok &= Expect(privilege_result->error_message == "non-root process has capabilities",
			             "capability failure diagnostic data is preserved");
		}
		ok &= Expect(context.events == std::vector<std::string>{"open", "drop"},
		             "capability-bearing non-root state skips engine and frame loop");
		return ok;
	}

	auto TestFilesystemIdentityFailuresStopProcessing() -> bool {
		struct FailureCase {
			const char *label;
			const char *error;
		};

		constexpr std::array<FailureCase, 4> cases{{
		    {.label = "root fsuid", .error = "inconsistent filesystem credentials"},
		    {.label = "root fsgid", .error = "inconsistent filesystem credentials"},
		    {.label = "mismatched fsuid", .error = "inconsistent filesystem credentials"},
		    {.label = "mismatched fsgid", .error = "inconsistent filesystem credentials"},
		}};

		bool ok = true;
		for (const auto &test_case : cases) {
			FakeContext context;
			context.privilege_status = ComparePrivilegeStatus::kVerificationFailure;
			context.privilege_error  = test_case.error;
			const auto  result       = Run(context);
			const auto *privilege_result =
			    std::get_if<howdy::native::ComparePrivilegeResult>(&result);
			ok &= Expect(privilege_result != nullptr,
			             std::string(test_case.label) + " returns privilege result");
			if (privilege_result != nullptr) {
				ok &=
				    Expect(privilege_result->status == ComparePrivilegeStatus::kVerificationFailure,
				           std::string(test_case.label) + " status is preserved");
				ok &= Expect(privilege_result->error_message == test_case.error,
				             std::string(test_case.label) + " diagnostic data is preserved");
			}
			ok &= Expect(context.events == std::vector<std::string>{"open", "drop"},
			             std::string(test_case.label) +
			                 " skips engine, timeout reset, and frame loop");
		}
		return ok;
	}

	auto TestProcessingResultPropagates() -> bool {
		FakeContext context;
		context.processing_exit = CompareExit::kTooDark;
		const auto result       = Run(context);

		bool        ok              = true;
		const auto *processing_exit = std::get_if<CompareExit>(&result);
		ok &= Expect(processing_exit != nullptr, "frame-loop completion returns frame-loop result");
		if (processing_exit != nullptr) {
			ok &= Expect(*processing_exit == CompareExit::kTooDark,
			             "frame-loop processing result propagates unchanged");
		}
		return ok;
	}

	auto TestModelFailure() -> bool {
		bool ok = true;
		for (const bool camera_fails : {false, true}) {
			FakeContext context;
			context.model_ready = false;
			context.open_status = camera_fails ? CompareCaptureOpenStatus::kOpenFailed
			                                   : CompareCaptureOpenStatus::kOk;
			const auto  result  = Run(context);
			const auto *exit    = std::get_if<CompareExit>(&result);
			ok &= Expect(exit != nullptr && *exit == CompareExit::kAbort,
			             "model failure aborts and takes precedence over camera failure");
			ok &=
			    Expect(context.model_finished && context.events == std::vector<std::string>{"open"},
			           "model failure joins worker and skips privilege drop and processing");
		}
		return ok;
	}

	auto TestStartupExceptions() -> bool {
		bool ok = true;
		for (const bool model_throws : {false, true}) {
			FakeContext context;
			context.model_throws  = model_throws;
			context.camera_throws = true;
			bool caught           = false;
			try {
				(void)Run(context);
			} catch (const std::runtime_error &error) {
				caught = std::string_view(error.what()) ==
				         (model_throws ? "model failed" : "camera threw");
			}
			ok &= Expect(caught, "startup exceptions propagate with model failure precedence");
			ok &=
			    Expect(context.model_finished && context.events == std::vector<std::string>{"open"},
			           "startup exception joins worker and skips privilege drop and processing");
		}
		return ok;
	}

}  // namespace

auto main() -> int {
	bool ok = true;
	ok &= TestMissingDependenciesFailClosed();
	ok &= TestSuccessfulPrivilegeValidationReachesProcessing();
	ok &= TestOpenFailure();
	ok &= TestModelFailure();
	ok &= TestStartupExceptions();
	ok &= TestInvalidCameraDependencies();
	ok &= TestNonRootCapabilityFailureStopsProcessing();
	ok &= TestFilesystemIdentityFailuresStopProcessing();
	ok &= TestProcessingResultPropagates();
	return ok ? 0 : 1;
}
