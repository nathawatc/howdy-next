#include "compare/capture_session.hpp"
#include "compare/privileges.hpp"
#include "compare/processing.hpp"
#include "protocol/compare_exit.hpp"
#include "test_support.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
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
		CompareCaptureOpenStatus                 open_status = CompareCaptureOpenStatus::kOk;
		howdy::native::CompareCaptureFrameStatus first_status =
		    howdy::native::CompareCaptureFrameStatus::kFrameReady;
		ComparePrivilegeStatus   privilege_status = ComparePrivilegeStatus::kOk;
		CompareExit              processing_exit  = CompareExit::kSuccess;
		std::string              privilege_error  = "non-root process has capabilities";
		std::vector<std::string> events;
		std::binary_semaphore    model_started{0};
		std::binary_semaphore    camera_opened{0};
		std::binary_semaphore    privilege_dropped{0};
		std::binary_semaphore    timeout_reset{0};
		std::binary_semaphore    warm_started{0};
		std::binary_semaphore    allow_warm_finish{0};
		std::binary_semaphore    first_read_seen{0};
		std::binary_semaphore    inference_started{0};
		std::binary_semaphore    before_encode_entered{0};
		std::binary_semaphore    encode_started{0};
		std::binary_semaphore    loop_returned{0};
		std::thread::id          main_thread               = std::this_thread::get_id();
		bool                     model_ready               = true;
		bool                     model_throws              = false;
		bool                     camera_throws             = false;
		bool                     model_finished            = false;
		bool                     model_on_worker           = false;
		bool                     camera_on_main            = false;
		bool                     startup_overlapped        = false;
		bool                     joined_before_drop        = false;
		bool                     warm_on_worker            = false;
		bool                     warm_after_drop_and_reset = false;
		bool                     read_on_main              = false;
		bool                     warm_seen_during_read     = false;
		std::atomic<bool>        warm_finished{false};
		bool                     joined_before_encode   = false;
		bool                     block_warm             = false;
		bool                     warm_throws            = false;
		bool                     warm_ready             = true;
		bool                     read_throws            = false;
		bool                     loop_throws            = false;
		bool                     simulate_encode        = true;
		int                      warm_calls             = 0;
		int                      encode_calls           = 0;
		int                      read_calls             = 0;
		int                      frame_processing_calls = 0;
		int                      processed_frame_number = 0;
		howdy::native::CompareCaptureFrameStatus processed_status =
		    howdy::native::CompareCaptureFrameStatus::kInvalidDependencies;
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
		context.privilege_dropped.release();
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
		context.timeout_reset.release();
	}

	auto WarmModel(void *raw_context) -> bool {
		auto &context          = *static_cast<FakeContext *>(raw_context);
		context.warm_on_worker = std::this_thread::get_id() != context.main_thread;
		context.warm_after_drop_and_reset =
		    context.privilege_dropped.try_acquire() && context.timeout_reset.try_acquire();
		++context.warm_calls;
		context.warm_started.release();
		if (context.block_warm &&
		    !context.allow_warm_finish.try_acquire_for(std::chrono::seconds(2))) {
			throw std::runtime_error("warm wait timed out");
		}
		context.warm_finished = true;
		if (context.warm_throws) {
			throw std::runtime_error("warm failed");
		}
		return context.warm_ready;
	}

	auto RunFrameLoop(void *raw_context, std::function<void()> &before_first_encode)
	    -> CompareExit {
		auto &context = *static_cast<FakeContext *>(raw_context);
		context.events.emplace_back("first next_frame");
		context.read_on_main = std::this_thread::get_id() == context.main_thread;
		context.warm_seen_during_read =
		    context.warm_started.try_acquire_for(std::chrono::seconds(2));
		++context.read_calls;
		context.first_read_seen.release();
		if (context.read_throws) {
			throw std::runtime_error("first read threw");
		}
		const howdy::native::CompareCaptureFrameResult capture_result{
		    .status       = context.first_status,
		    .frame_number = 1,
		};
		context.events.emplace_back("first inference");
		context.inference_started.release();
		++context.frame_processing_calls;
		context.processed_frame_number = capture_result.frame_number;
		context.processed_status       = capture_result.status;
		if (capture_result.status == howdy::native::CompareCaptureFrameStatus::kFrameReady &&
		    context.simulate_encode) {
			context.before_encode_entered.release();
			before_first_encode();
			context.joined_before_encode = context.warm_finished;
			++context.encode_calls;
			context.encode_started.release();
		}
		if (context.loop_throws) {
			throw std::runtime_error("frame loop threw");
		}
		context.loop_returned.release();
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
		    .warm_model       = WarmModel,
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
		dependencies            = DependenciesFor(context);
		dependencies.warm_model = nullptr;
		expect_abort(dependencies, "missing warm model");
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
		                                                        "first next_frame",
		                                                        "first inference"},
		             "successful privilege validation reaches every processing stage");
		ok &=
		    Expect(context.startup_overlapped && context.model_on_worker && context.camera_on_main,
		           "model initialization overlaps camera setup on the main thread");
		ok &= Expect(context.joined_before_drop, "model worker finishes before privilege drop");
		ok &= Expect(context.warm_on_worker && context.read_on_main &&
		                 context.warm_seen_during_read && context.joined_before_encode &&
		                 context.warm_after_drop_and_reset,
		             "SFace warm-up starts after drop and joins before first encode");
		ok &= Expect(context.warm_calls == 1 && context.read_calls == 1 &&
		                 context.frame_processing_calls == 1 && context.processed_frame_number == 1,
		             "first frame is read and processed exactly once with frame number one");
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

	auto TestImmediateReadContinuesWhileWarmBlocked() -> bool {
		FakeContext context;
		context.block_warm                           = true;
		bool         read_seen                       = false;
		bool         loop_started_while_warm_blocked = false;
		std::jthread release_worker([&]() -> void {
			read_seen = context.first_read_seen.try_acquire_for(std::chrono::seconds(2));
			if (read_seen) {
				loop_started_while_warm_blocked =
				    context.inference_started.try_acquire_for(std::chrono::seconds(2)) &&
				    !context.warm_finished;
			}
			context.allow_warm_finish.release();
		});
		const auto   result = Run(context);
		release_worker.join();
		bool ok = true;
		ok &= Expect(read_seen && loop_started_while_warm_blocked,
		             "immediate first read enters frame loop while SFace warm-up is blocked");
		ok &= Expect(std::get_if<CompareExit>(&result) != nullptr && context.joined_before_encode &&
		                 context.warm_finished,
		             "first encode waits for blocked SFace warm-up");
		return ok;
	}

	auto TestFirstEncodeWaitsForWarmUp() -> bool {
		FakeContext context;
		context.block_warm          = true;
		bool         reached_encode = false;
		bool         encoded_early  = false;
		std::jthread release_worker([&]() -> void {
			reached_encode = context.before_encode_entered.try_acquire_for(std::chrono::seconds(2));
			if (reached_encode) {
				encoded_early =
				    context.encode_started.try_acquire_for(std::chrono::milliseconds(30));
			}
			context.allow_warm_finish.release();
		});
		const auto   result = Run(context);
		release_worker.join();
		return Expect(reached_encode && !encoded_early && context.joined_before_encode &&
		                  context.encode_calls == 1 && std::get_if<CompareExit>(&result) != nullptr,
		              "first encode waits; real encode begins only after warm-up ends");
	}

	auto TestNoFaceSkipsWarmWait() -> bool {
		FakeContext context;
		context.block_warm                            = true;
		context.simulate_encode                       = false;
		bool         loop_returned_while_warm_blocked = false;
		std::jthread release_worker([&]() -> void {
			loop_returned_while_warm_blocked =
			    context.loop_returned.try_acquire_for(std::chrono::seconds(2)) &&
			    !context.warm_finished;
			context.allow_warm_finish.release();
		});
		const auto   result = Run(context);
		release_worker.join();
		return Expect(loop_returned_while_warm_blocked && context.encode_calls == 0 &&
		                  context.warm_finished && std::get_if<CompareExit>(&result) != nullptr,
		              "no-face loop skips warm wait; cleanup joins worker");
	}

	auto TestOptionalWarmFailureAndFirstReadExits() -> bool {
		bool ok = true;
		for (const bool throws : {false, true}) {
			FakeContext context;
			context.warm_ready  = false;
			context.warm_throws = throws;
			const auto result   = Run(context);
			ok &= Expect(std::get_if<CompareExit>(&result) != nullptr &&
			                 context.joined_before_encode && context.frame_processing_calls == 1,
			             "optional warm failure still processes first frame");
		}
		for (const auto status : {howdy::native::CompareCaptureFrameStatus::kReadFailed,
		                          howdy::native::CompareCaptureFrameStatus::kTimeout,
		                          howdy::native::CompareCaptureFrameStatus::kNotOpen,
		                          howdy::native::CompareCaptureFrameStatus::kInvalidDependencies}) {
			FakeContext context;
			context.first_status                          = status;
			context.block_warm                            = true;
			bool         loop_returned_while_warm_blocked = false;
			std::jthread release_worker([&]() -> void {
				loop_returned_while_warm_blocked =
				    context.loop_returned.try_acquire_for(std::chrono::seconds(2)) &&
				    !context.warm_finished;
				context.allow_warm_finish.release();
			});
			const auto   result = Run(context);
			release_worker.join();
			ok &= Expect(std::get_if<CompareExit>(&result) != nullptr && context.warm_finished &&
			                 loop_returned_while_warm_blocked && context.encode_calls == 0 &&
			                 context.processed_status == status &&
			                 context.processed_frame_number == 1 && context.read_calls == 1,
			             "failed first capture does not wait; cleanup joins warm worker");
		}
		{
			FakeContext context;
			context.read_throws = true;
			context.block_warm  = true;
			std::jthread release_worker([&]() -> void {
				(void)context.first_read_seen.try_acquire_for(std::chrono::seconds(2));
				context.allow_warm_finish.release();
			});
			bool         caught = false;
			try {
				(void)Run(context);
			} catch (const std::runtime_error &error) {
				caught = std::string_view(error.what()) == "first read threw";
			}
			release_worker.join();
			ok &= Expect(caught && context.warm_finished,
			             "throwing first read joins warm worker before exception propagates");
		}
		{
			FakeContext context;
			context.loop_throws     = true;
			context.simulate_encode = false;
			context.block_warm      = true;
			std::jthread release_worker([&]() -> void {
				(void)context.inference_started.try_acquire_for(std::chrono::seconds(2));
				context.allow_warm_finish.release();
			});
			bool         caught = false;
			try {
				(void)Run(context);
			} catch (const std::runtime_error &error) {
				caught = std::string_view(error.what()) == "frame loop threw";
			}
			release_worker.join();
			ok &= Expect(caught && context.warm_finished,
			             "throwing frame loop joins warm worker before exception propagates");
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
	ok &= TestImmediateReadContinuesWhileWarmBlocked();
	ok &= TestFirstEncodeWaitsForWarmUp();
	ok &= TestNoFaceSkipsWarmWait();
	ok &= TestOptionalWarmFailureAndFirstReadExits();
	return ok ? 0 : 1;
}
