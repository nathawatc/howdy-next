#include "compare/processing_internal.hpp"
#include "test_support.hpp"

#include <array>
#include <string>
#include <vector>

namespace {

	using howdy::test::expect;

	using howdy::native::CompareCaptureOpenStatus;
	using howdy::native::CompareExit;
	using howdy::native::ComparePrivilegeStatus;
	using howdy::native::compare_processing_internal::CompareProcessingDependencies;
	using howdy::native::compare_processing_internal::CompareProcessingInvalidDependencies;
	using howdy::native::compare_processing_internal::CompareProcessingResult;

	struct FakeContext {
		CompareCaptureOpenStatus open_status      = CompareCaptureOpenStatus::kOk;
		ComparePrivilegeStatus   privilege_status = ComparePrivilegeStatus::kOk;
		CompareExit              processing_exit  = CompareExit::kSuccess;
		std::string              privilege_error  = "non-root process has capabilities";
		std::vector<std::string> events;
	};

	auto open_capture(void *raw_context) -> howdy::native::CompareCaptureOpenResult {
		auto &context = *static_cast<FakeContext *>(raw_context);
		context.events.emplace_back("open");
		return {
		    .status = context.open_status,
		    .error_message =
		        context.open_status == CompareCaptureOpenStatus::kOpenFailed ? "camera failed" : "",
		};
	}

	auto drop_privileges(void *raw_context) -> howdy::native::ComparePrivilegeResult {
		auto &context = *static_cast<FakeContext *>(raw_context);
		context.events.emplace_back("drop");
		return {
		    .status        = context.privilege_status,
		    .error_message = context.privilege_status == ComparePrivilegeStatus::kOk
		                         ? ""
		                         : context.privilege_error,
		};
	}

	void construct_engine(void *raw_context) {
		auto &context = *static_cast<FakeContext *>(raw_context);
		context.events.emplace_back("engine");
	}

	void reset_timeout(void *raw_context) {
		auto &context = *static_cast<FakeContext *>(raw_context);
		context.events.emplace_back("reset");
	}

	auto run_frame_loop(void *raw_context) -> CompareExit {
		auto &context = *static_cast<FakeContext *>(raw_context);
		context.events.emplace_back("first next_frame");
		return context.processing_exit;
	}

	auto dependencies_for(FakeContext &context) -> CompareProcessingDependencies {
		return {
		    .context          = &context,
		    .open_capture     = open_capture,
		    .drop_privileges  = drop_privileges,
		    .construct_engine = construct_engine,
		    .reset_timeout    = reset_timeout,
		    .run_frame_loop   = run_frame_loop,
		};
	}

	auto run(FakeContext &context) -> CompareProcessingResult {
		return howdy::native::compare_processing_internal::run_compare_processing(
		    dependencies_for(context));
	}

	auto test_successful_privilege_validation_reaches_processing() -> bool {
		FakeContext context;
		const auto  result = run(context);

		bool        ok              = true;
		const auto *processing_exit = std::get_if<CompareExit>(&result);
		ok &= expect(processing_exit != nullptr, "successful validation returns frame-loop result");
		if (processing_exit != nullptr) {
			ok &= expect(*processing_exit == CompareExit::kSuccess,
			             "successful frame-loop result propagates");
		}
		ok &= expect(context.events == std::vector<std::string>{"open", "drop", "engine", "reset",
		                                                        "first next_frame"},
		             "successful privilege validation reaches every processing stage");
		return ok;
	}

	auto test_open_failure() -> bool {
		FakeContext context;
		context.open_status = CompareCaptureOpenStatus::kOpenFailed;
		const auto result   = run(context);

		bool        ok             = true;
		const auto *capture_result = std::get_if<howdy::native::CompareCaptureOpenResult>(&result);
		ok &= expect(capture_result != nullptr, "open failure returns capture result");
		if (capture_result != nullptr) {
			ok &= expect(capture_result->status == CompareCaptureOpenStatus::kOpenFailed,
			             "open failure status is preserved");
			ok &=
			    expect(capture_result->error_message == "camera failed", "open error is preserved");
		}
		ok &= expect(context.events == std::vector<std::string>{"open"},
		             "open failure skips all later stages");
		return ok;
	}

	auto test_invalid_camera_dependencies() -> bool {
		FakeContext context;
		context.open_status = CompareCaptureOpenStatus::kInvalidDependencies;
		const auto result   = run(context);

		bool        ok             = true;
		const auto *capture_result = std::get_if<howdy::native::CompareCaptureOpenResult>(&result);
		ok &=
		    expect(capture_result != nullptr, "invalid camera dependencies return capture result");
		if (capture_result != nullptr) {
			ok &= expect(capture_result->status == CompareCaptureOpenStatus::kInvalidDependencies,
			             "invalid camera dependency status is preserved");
		}
		ok &= expect(context.events == std::vector<std::string>{"open"},
		             "invalid camera dependencies skip all later stages");
		return ok;
	}

	auto test_non_root_capability_failure_stops_processing() -> bool {
		FakeContext context;
		context.privilege_status = ComparePrivilegeStatus::kVerificationFailure;
		const auto result        = run(context);

		bool        ok               = true;
		const auto *privilege_result = std::get_if<howdy::native::ComparePrivilegeResult>(&result);
		ok &= expect(privilege_result != nullptr, "capability failure returns privilege result");
		if (privilege_result != nullptr) {
			ok &= expect(privilege_result->status == ComparePrivilegeStatus::kVerificationFailure,
			             "capability failure status is preserved");
			ok &= expect(privilege_result->error_message == "non-root process has capabilities",
			             "capability failure diagnostic data is preserved");
		}
		ok &= expect(context.events == std::vector<std::string>{"open", "drop"},
		             "capability-bearing non-root state skips engine and frame loop");
		return ok;
	}

	auto test_filesystem_identity_failures_stop_processing() -> bool {
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
			const auto  result       = run(context);
			const auto *privilege_result =
			    std::get_if<howdy::native::ComparePrivilegeResult>(&result);
			ok &= expect(privilege_result != nullptr,
			             std::string(test_case.label) + " returns privilege result");
			if (privilege_result != nullptr) {
				ok &=
				    expect(privilege_result->status == ComparePrivilegeStatus::kVerificationFailure,
				           std::string(test_case.label) + " status is preserved");
				ok &= expect(privilege_result->error_message == test_case.error,
				             std::string(test_case.label) + " diagnostic data is preserved");
			}
			ok &= expect(context.events == std::vector<std::string>{"open", "drop"},
			             std::string(test_case.label) +
			                 " skips engine, timeout reset, and frame loop");
		}
		return ok;
	}

	auto test_processing_result_propagates() -> bool {
		FakeContext context;
		context.processing_exit = CompareExit::kTooDark;
		const auto result       = run(context);

		bool        ok              = true;
		const auto *processing_exit = std::get_if<CompareExit>(&result);
		ok &= expect(processing_exit != nullptr, "frame-loop completion returns frame-loop result");
		if (processing_exit != nullptr) {
			ok &= expect(*processing_exit == CompareExit::kTooDark,
			             "frame-loop processing result propagates unchanged");
		}
		return ok;
	}

	auto test_missing_callbacks() -> bool {
		bool ok = true;
		for (int missing = 0; missing < 5; missing++) {
			FakeContext context;
			auto        dependencies = dependencies_for(context);
			switch (missing) {
				case 0:
					dependencies.open_capture = nullptr;
					break;
				case 1:
					dependencies.drop_privileges = nullptr;
					break;
				case 2:
					dependencies.construct_engine = nullptr;
					break;
				case 3:
					dependencies.reset_timeout = nullptr;
					break;
				case 4:
					dependencies.run_frame_loop = nullptr;
					break;
				default:
					break;
			}

			const auto result =
			    howdy::native::compare_processing_internal::run_compare_processing(dependencies);
			ok &= expect(std::holds_alternative<CompareProcessingInvalidDependencies>(result),
			             "missing callback returns invalid-dependencies stage result");
			ok &= expect(context.events.empty(),
			             "all callbacks validate before orchestration starts");
		}
		return ok;
	}

}  // namespace

auto main() -> int {
	bool ok = true;
	ok &= test_successful_privilege_validation_reaches_processing();
	ok &= test_open_failure();
	ok &= test_invalid_camera_dependencies();
	ok &= test_non_root_capability_failure_stops_processing();
	ok &= test_filesystem_identity_failures_stop_processing();
	ok &= test_processing_result_propagates();
	ok &= test_missing_callbacks();
	return ok ? 0 : 1;
}
