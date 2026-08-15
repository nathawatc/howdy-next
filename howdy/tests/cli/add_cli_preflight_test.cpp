#include "cli/add_cli_test_support.hpp"

#include <iostream>
#include <sstream>

namespace howdy::test::add_cli {

	namespace {

		auto invalid_config_load_result() -> howdy::native::RuntimeConfigLoadResult {
			return howdy::native::RuntimeConfigLoadResult{
			    .ok            = false,
			    .status        = howdy::native::RuntimeConfigLoadStatus::kInvalidRuntimeValue,
			    .error_message = "invalid runtime config",
			};
		}

		auto invalid_runtime_config_stops_before_preflight() -> bool {
			auto context          = make_success_context();
			context.config_result = invalid_config_load_result();

			const int result = run_add(context, {"howdy-add", "alice", "front-door"});

			bool ok = true;
			ok &= expect(result == 1, "invalid runtime config returns 1");
			ok &= expect(context.load_calls == 1, "runtime config callback called");
			ok &= expect(context.preflight_calls == 0,
			             "invalid runtime config skips preflight callback");
			ok &=
			    expect(context.capture_calls == 0, "invalid runtime config skips capture callback");
			ok &= expect(context.append_calls == 0, "invalid runtime config skips append callback");
			return ok;
		}

		struct PreflightFailureCase {
			howdy::native::add_internal::AddPreflightStatus status;
			std::string                                     message;
			std::string                                     test_name;
		};

		auto preflight_failure_stops_before_prompt(const PreflightFailureCase &test_case) -> bool {
			auto context             = make_success_context();
			context.preflight_result = howdy::native::add_internal::AddPreflightResult{
			    .status        = test_case.status,
			    .error_message = test_case.message,
			};
			std::istringstream input("front-door\n");
			std::ostringstream output;
			context.input_stream = &input;
			StreamRedirect redirect(std::cin, input.rdbuf(), std::cout, output.rdbuf());

			const int result = run_add(context, {"howdy-add", "alice"});

			bool ok = true;
			ok &= expect(result == 1, test_case.test_name + " returns 1");
			ok &= expect(context.preflight_calls == 1, test_case.test_name + " calls preflight");
			ok &= expect(context.preflight_saw_unread_input,
			             test_case.test_name + " runs preflight before prompt");
			ok &= expect(context.capture_calls == 0, test_case.test_name + " skips capture");
			ok &= expect(context.append_calls == 0, test_case.test_name + " skips append");
			ok &= expect(input.tellg() == std::streampos(0),
			             test_case.test_name + " leaves label input unread");
			ok &= expect(!output.str().contains("Enter a label for this new model"),
			             test_case.test_name + " does not prompt for label");
			return ok;
		}

		auto face_model_preflight_failure_stops_before_prompt() -> bool {
			return preflight_failure_stops_before_prompt(
			    {.status    = howdy::native::add_internal::AddPreflightStatus::kFaceModelError,
			     .message   = "face failed",
			     .test_name = "face-model preflight failure"});
		}

		auto incompatible_existing_model_stops_before_prompt() -> bool {
			return preflight_failure_stops_before_prompt(
			    {.status =
			         howdy::native::add_internal::AddPreflightStatus::kExistingModelIncompatible,
			     .message   = "",
			     .test_name = "incompatible existing model"});
		}

		auto existing_model_error_stops_before_prompt() -> bool {
			return preflight_failure_stops_before_prompt(
			    {.status    = howdy::native::add_internal::AddPreflightStatus::kExistingModelError,
			     .message   = "storage failed",
			     .test_name = "existing model preflight error"});
		}

		auto unknown_preflight_status_fails_closed_before_prompt() -> bool {
			auto context             = make_success_context();
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

			const int result = run_add(context, {"howdy-add", "alice"});

			bool ok = true;
			ok &= expect(result == 1, "unknown preflight status returns 1");
			ok &= expect(context.preflight_calls == 1, "unknown preflight status calls preflight");
			ok &= expect(context.capture_calls == 0, "unknown preflight status skips capture");
			ok &= expect(context.append_calls == 0, "unknown preflight status skips append");
			ok &= expect(input.tellg() == std::streampos(0),
			             "unknown preflight status leaves label input unread");
			ok &= expect(!output.str().contains("Enter a label for this new model"),
			             "unknown preflight status does not prompt for label");
			ok &= expect(error.str().contains("Internal error: unknown add preflight status"),
			             "unknown preflight status writes internal error");
			return ok;
		}

		auto incomplete_dependencies_stop_before_callbacks() -> bool {
			auto context = make_success_context();

			const int result = run_add_with_dependencies(
			    howdy::native::add_internal::AddDependencies{
			        .context              = &context,
			        .load_runtime_config  = load_runtime_config_callback,
			        .preflight_enrollment = nullptr,
			        .capture_enrollment   = capture_enrollment_callback,
			        .append_user_model    = append_user_model_entry_callback,
			    },
			    {"howdy-add", "alice", "front-door"});

			bool ok = true;
			ok &= expect(result == 1, "incomplete dependencies return 1");
			ok &= expect(context.load_calls == 0,
			             "incomplete dependencies skip available runtime config callback");
			ok &= expect(context.preflight_calls == 0,
			             "incomplete dependencies skip preflight callback");
			ok &= expect(context.capture_calls == 0,
			             "incomplete dependencies skip available capture callback");
			ok &= expect(context.append_calls == 0,
			             "incomplete dependencies skip available append callback");
			return ok;
		}

	}  // namespace

	auto run_add_cli_preflight_tests() -> bool {
		bool ok = true;
		ok &= invalid_runtime_config_stops_before_preflight();
		ok &= face_model_preflight_failure_stops_before_prompt();
		ok &= incompatible_existing_model_stops_before_prompt();
		ok &= existing_model_error_stops_before_prompt();
		ok &= unknown_preflight_status_fails_closed_before_prompt();
		return ok;
	}

	auto run_add_cli_dependency_validation_tests() -> bool {
		return incomplete_dependencies_stop_before_callbacks();
	}

}  // namespace howdy::test::add_cli
