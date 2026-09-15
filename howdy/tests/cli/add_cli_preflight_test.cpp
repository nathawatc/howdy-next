#include "cli/add_cli_test_support.hpp"

#include <iostream>
#include <sstream>

namespace howdy::test::add_cli {

	namespace {

		auto InvalidConfigLoadResult() -> howdy::native::RuntimeConfigLoadResult {
			return howdy::native::RuntimeConfigLoadResult{
			    .ok            = false,
			    .status        = howdy::native::RuntimeConfigLoadStatus::kInvalidRuntimeValue,
			    .error_message = "invalid runtime config",
			};
		}

		auto InvalidRuntimeConfigStopsBeforePreflight() -> bool {
			auto context          = MakeSuccessContext();
			context.config_result = InvalidConfigLoadResult();

			const int result = RunAdd(context, {"howdy-add", "alice", "front-door"});

			bool ok = true;
			ok &= Expect(result == 1, "invalid runtime config returns 1");
			ok &= Expect(context.load_calls == 1, "runtime config callback called");
			ok &= Expect(context.preflight_calls == 0,
			             "invalid runtime config skips preflight callback");
			ok &=
			    Expect(context.capture_calls == 0, "invalid runtime config skips capture callback");
			ok &= Expect(context.append_calls == 0, "invalid runtime config skips append callback");
			return ok;
		}

		struct PreflightFailureCase {
			howdy::native::add_internal::AddPreflightStatus status;
			std::string                                     message;
			std::string                                     test_name;
		};

		auto PreflightFailureStopsBeforePrompt(const PreflightFailureCase &test_case) -> bool {
			auto context             = MakeSuccessContext();
			context.preflight_result = howdy::native::add_internal::AddPreflightResult{
			    .status        = test_case.status,
			    .error_message = test_case.message,
			};
			std::istringstream input("front-door\n");
			std::ostringstream output;
			context.input_stream = &input;
			StreamRedirect redirect(std::cin, input.rdbuf(), std::cout, output.rdbuf());

			const int result = RunAdd(context, {"howdy-add", "alice"});

			bool ok = true;
			ok &= Expect(result == 1, test_case.test_name + " returns 1");
			ok &= Expect(context.preflight_calls == 1, test_case.test_name + " calls preflight");
			ok &= Expect(context.preflight_saw_unread_input,
			             test_case.test_name + " runs preflight before prompt");
			ok &= Expect(context.capture_calls == 0, test_case.test_name + " skips capture");
			ok &= Expect(context.append_calls == 0, test_case.test_name + " skips append");
			ok &= Expect(input.tellg() == std::streampos(0),
			             test_case.test_name + " leaves label input unread");
			ok &= Expect(!output.str().contains("Enter a label for this new model"),
			             test_case.test_name + " does not prompt for label");
			return ok;
		}

		auto FaceModelPreflightFailureStopsBeforePrompt() -> bool {
			return PreflightFailureStopsBeforePrompt(
			    {.status    = howdy::native::add_internal::AddPreflightStatus::kFaceModelError,
			     .message   = "face failed",
			     .test_name = "face-model preflight failure"});
		}

		auto IncompatibleExistingModelStopsBeforePrompt() -> bool {
			return PreflightFailureStopsBeforePrompt(
			    {.status =
			         howdy::native::add_internal::AddPreflightStatus::kExistingModelIncompatible,
			     .message   = "",
			     .test_name = "incompatible existing model"});
		}

		auto ExistingModelErrorStopsBeforePrompt() -> bool {
			return PreflightFailureStopsBeforePrompt(
			    {.status    = howdy::native::add_internal::AddPreflightStatus::kExistingModelError,
			     .message   = "storage failed",
			     .test_name = "existing model preflight error"});
		}

		auto UnknownPreflightStatusFailsClosedBeforePrompt() -> bool {
			auto context             = MakeSuccessContext();
			context.preflight_result = howdy::native::add_internal::AddPreflightResult{
			    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
			    .status = static_cast<howdy::native::add_internal::AddPreflightStatus>(99),
			};
			std::istringstream input("front-door\n");
			std::ostringstream output;
			std::ostringstream error;
			context.input_stream = &input;
			StreamRedirect redirect(std::cin, input.rdbuf(), std::cout, output.rdbuf());
			ErrorRedirect  error_redirect(std::cerr, error.rdbuf());

			const int result = RunAdd(context, {"howdy-add", "alice"});

			bool ok = true;
			ok &= Expect(result == 1, "unknown preflight status returns 1");
			ok &= Expect(context.preflight_calls == 1, "unknown preflight status calls preflight");
			ok &= Expect(context.capture_calls == 0, "unknown preflight status skips capture");
			ok &= Expect(context.append_calls == 0, "unknown preflight status skips append");
			ok &= Expect(input.tellg() == std::streampos(0),
			             "unknown preflight status leaves label input unread");
			ok &= Expect(!output.str().contains("Enter a label for this new model"),
			             "unknown preflight status does not prompt for label");
			ok &= Expect(error.str().contains("Internal error: unknown add preflight status"),
			             "unknown preflight status writes internal error");
			return ok;
		}

		auto IncompleteDependenciesStopBeforeCallbacks() -> bool {
			auto context = MakeSuccessContext();

			const int result = RunAddWithDependencies(
			    howdy::native::add_internal::AddDependencies{
			        .context              = &context,
			        .load_runtime_config  = LoadRuntimeConfigCallback,
			        .preflight_enrollment = nullptr,
			        .capture_enrollment   = CaptureEnrollmentCallback,
			        .append_user_model    = AppendUserModelEntryCallback,
			    },
			    {"howdy-add", "alice", "front-door"});

			bool ok = true;
			ok &= Expect(result == 1, "incomplete dependencies return 1");
			ok &= Expect(context.load_calls == 0,
			             "incomplete dependencies skip available runtime config callback");
			ok &= Expect(context.preflight_calls == 0,
			             "incomplete dependencies skip preflight callback");
			ok &= Expect(context.capture_calls == 0,
			             "incomplete dependencies skip available capture callback");
			ok &= Expect(context.append_calls == 0,
			             "incomplete dependencies skip available append callback");
			return ok;
		}

	}  // namespace

	auto RunAddCliPreflightTests() -> bool {
		bool ok = true;
		ok &= InvalidRuntimeConfigStopsBeforePreflight();
		ok &= FaceModelPreflightFailureStopsBeforePrompt();
		ok &= IncompatibleExistingModelStopsBeforePrompt();
		ok &= ExistingModelErrorStopsBeforePrompt();
		ok &= UnknownPreflightStatusFailsClosedBeforePrompt();
		return ok;
	}

	auto RunAddCliDependencyValidationTests() -> bool {
		return IncompleteDependenciesStopBeforeCallbacks();
	}

}  // namespace howdy::test::add_cli
