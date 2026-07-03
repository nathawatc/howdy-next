#include "cli/add_internal.hpp"
#include "core/face_model.hpp"

#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

	struct StreamRedirect {
		StreamRedirect(std::istream &input_stream, std::streambuf *new_input,
		               std::ostream &output_stream, std::streambuf *new_output)
		    : input(input_stream)
		    , output(output_stream)
		    , old_input(input_stream.rdbuf(new_input))
		    , old_output(output_stream.rdbuf(new_output)) {}

		~StreamRedirect() {
			input.rdbuf(old_input);
			output.rdbuf(old_output);
		}

		std::istream   &input;
		std::ostream   &output;
		std::streambuf *old_input;
		std::streambuf *old_output;
	};

	struct ErrorRedirect {
		explicit ErrorRedirect(std::ostream &error_stream, std::streambuf *new_error)
		    : error(error_stream)
		    , old_error(error_stream.rdbuf(new_error)) {}

		~ErrorRedirect() {
			error.rdbuf(old_error);
		}

		std::ostream   &error;
		std::streambuf *old_error;
	};

	struct AddCliTestContext {
		howdy::native::RuntimeConfigLoadResult           config_result;
		howdy::native::add_internal::AddPreflightResult  preflight_result;
		howdy::native::add_internal::AddEnrollmentResult capture_result;
		howdy::native::UserModelMutationResult           append_result;
		int                                              load_calls                 = 0;
		int                                              preflight_calls            = 0;
		int                                              capture_calls              = 0;
		int                                              append_calls               = 0;
		bool                                             capture_plain              = false;
		bool                                             preflight_saw_unread_input = false;
		std::istringstream                              *input_stream               = nullptr;
		std::string                                      preflight_user;
		howdy::native::RuntimeConfig                     preflight_config;
		std::string                                      capture_user;
		howdy::native::RuntimeConfig                     capture_config;
		std::string                                      capture_label;
		std::string                                      appended_user;
		howdy::native::NewUserModelEntry                 appended_entry;
		std::vector<std::string>                         events;
	};

	auto expect(bool condition, const std::string &message) -> bool {
		if (!condition) {
			std::cerr << "FAIL: " << message << "\n";
			return false;
		}
		return true;
	}

	auto valid_config_load_result() -> howdy::native::RuntimeConfigLoadResult {
		howdy::native::RuntimeConfig config;
		config.video.dark_threshold = 32.0F;
		return howdy::native::RuntimeConfigLoadResult{
		    .ok     = true,
		    .status = howdy::native::RuntimeConfigLoadStatus::kOk,
		    .config = config,
		};
	}

	auto invalid_config_load_result() -> howdy::native::RuntimeConfigLoadResult {
		return howdy::native::RuntimeConfigLoadResult{
		    .ok            = false,
		    .status        = howdy::native::RuntimeConfigLoadStatus::kInvalidRuntimeValue,
		    .error_message = "invalid runtime config",
		};
	}

	auto successful_preflight_result() -> howdy::native::add_internal::AddPreflightResult {
		return howdy::native::add_internal::AddPreflightResult{
		    .status = howdy::native::add_internal::AddPreflightStatus::kOk,
		};
	}

	auto successful_capture_result() -> howdy::native::add_internal::AddEnrollmentResult {
		return howdy::native::add_internal::AddEnrollmentResult{
		    .status   = howdy::native::add_internal::AddEnrollmentStatus::kOk,
		    .metric   = "cosine",
		    .encoding = {0.125F},
		};
	}

	auto capture_failure_result(howdy::native::EnrollmentCaptureResult capture_result)
	    -> howdy::native::add_internal::AddEnrollmentResult {
		return howdy::native::add_internal::AddEnrollmentResult{
		    .status         = howdy::native::add_internal::AddEnrollmentStatus::kCaptureFailure,
		    .capture_result = std::move(capture_result),
		};
	}

	auto black_frame_capture_result() -> howdy::native::add_internal::AddEnrollmentResult {
		howdy::native::EnrollmentCaptureResult capture_result;
		capture_result.black_frames = 1;
		return capture_failure_result(std::move(capture_result));
	}

	auto only_too_dark_capture_result() -> howdy::native::add_internal::AddEnrollmentResult {
		howdy::native::EnrollmentCaptureResult capture_result;
		capture_result.valid_frames       = 2;
		capture_result.dark_tries         = 2;
		capture_result.black_frames       = 0;
		capture_result.empty_frames       = 0;
		capture_result.read_failures      = 0;
		capture_result.dark_running_total = 80.0;
		return capture_failure_result(std::move(capture_result));
	}

	auto no_sufficiently_bright_capture_result()
	    -> howdy::native::add_internal::AddEnrollmentResult {
		howdy::native::EnrollmentCaptureResult capture_result;
		capture_result.valid_frames  = 1;
		capture_result.dark_tries    = 1;
		capture_result.black_frames  = 0;
		capture_result.empty_frames  = 1;
		capture_result.read_failures = 0;
		return capture_failure_result(std::move(capture_result));
	}

	auto no_usable_frames_capture_result() -> howdy::native::add_internal::AddEnrollmentResult {
		howdy::native::EnrollmentCaptureResult capture_result;
		capture_result.valid_frames  = 0;
		capture_result.read_failures = 1;
		return capture_failure_result(std::move(capture_result));
	}

	auto no_face_detected_capture_result() -> howdy::native::add_internal::AddEnrollmentResult {
		howdy::native::EnrollmentCaptureResult capture_result;
		capture_result.valid_frames = 1;
		capture_result.dark_tries   = 0;
		return capture_failure_result(std::move(capture_result));
	}

	auto enrollment_failure_result(howdy::native::add_internal::AddEnrollmentStatus status)
	    -> howdy::native::add_internal::AddEnrollmentResult {
		return howdy::native::add_internal::AddEnrollmentResult{
		    .status        = status,
		    .error_message = "enrollment failed",
		};
	}

	auto load_runtime_config_callback(void *raw_context) -> howdy::native::RuntimeConfigLoadResult {
		auto *context = static_cast<AddCliTestContext *>(raw_context);
		++context->load_calls;
		context->events.emplace_back("load");
		return context->config_result;
	}

	auto preflight_enrollment_callback(void *raw_context, const std::string &user,
	                                   const howdy::native::RuntimeConfig &config)
	    -> howdy::native::add_internal::AddPreflightResult {
		auto *context = static_cast<AddCliTestContext *>(raw_context);
		++context->preflight_calls;
		context->events.emplace_back("preflight");
		context->preflight_user   = user;
		context->preflight_config = config;
		if (context->input_stream != nullptr) {
			context->preflight_saw_unread_input =
			    context->input_stream->tellg() == std::streampos(0);
		}
		return context->preflight_result;
	}

	auto capture_enrollment_callback(void *raw_context, const std::string &user,
	                                 const howdy::native::RuntimeConfig &config, bool plain,
	                                 const std::string &label)
	    -> howdy::native::add_internal::AddEnrollmentResult {
		auto *context = static_cast<AddCliTestContext *>(raw_context);
		++context->capture_calls;
		context->events.emplace_back("capture");
		context->capture_user   = user;
		context->capture_config = config;
		context->capture_plain  = plain;
		context->capture_label  = label;
		return context->capture_result;
	}

	auto append_user_model_entry_callback(void *raw_context, const std::string &user,
	                                      const howdy::native::NewUserModelEntry &entry)
	    -> howdy::native::UserModelMutationResult {
		auto *context = static_cast<AddCliTestContext *>(raw_context);
		++context->append_calls;
		context->events.emplace_back("append");
		context->appended_user  = user;
		context->appended_entry = entry;
		return context->append_result;
	}

	auto test_dependencies(AddCliTestContext &context)
	    -> howdy::native::add_internal::AddDependencies {
		return howdy::native::add_internal::AddDependencies{
		    .context              = &context,
		    .load_runtime_config  = load_runtime_config_callback,
		    .preflight_enrollment = preflight_enrollment_callback,
		    .capture_enrollment   = capture_enrollment_callback,
		    .append_user_model    = append_user_model_entry_callback,
		};
	}

	auto run_add_with_dependencies(howdy::native::add_internal::AddDependencies dependencies,
	                               std::vector<std::string>                     arguments) -> int {
		std::vector<char *> argv;
		argv.reserve(arguments.size());
		for (auto &argument : arguments) {
			argv.push_back(argument.data());
		}
		return howdy::native::add_internal::add_main_with_dependencies(
		    static_cast<int>(argv.size()), argv.data(), dependencies);
	}

	auto run_add(AddCliTestContext &context, std::vector<std::string> arguments) -> int {
		return run_add_with_dependencies(test_dependencies(context), std::move(arguments));
	}

	auto make_success_context() -> AddCliTestContext {
		AddCliTestContext context;
		context.config_result    = valid_config_load_result();
		context.preflight_result = successful_preflight_result();
		context.capture_result   = successful_capture_result();
		context.append_result    = howdy::native::UserModelMutationResult{
		    .status = howdy::native::UserModelStatus::kOk,
		};
		return context;
	}

	auto successful_enrollment_appends_expected_model() -> bool {
		auto      context = make_success_context();
		const int result  = run_add(context, {"howdy-add", "alice", "front-door"});

		bool ok = true;
		ok &= expect(result == 0, "successful add returns 0");
		ok &= expect(context.load_calls == 1, "runtime config loaded once");
		ok &= expect(context.preflight_calls == 1, "preflight callback called once");
		ok &= expect(context.preflight_user == "alice", "preflight callback receives user");
		ok &= expect(context.preflight_config.video.dark_threshold == 32.0F,
		             "preflight callback receives runtime config");
		ok &= expect(context.capture_calls == 1, "capture callback called once");
		ok &= expect(!context.capture_plain, "non-plain flag reaches capture callback");
		ok &= expect(context.capture_user == "alice", "capture callback receives user");
		ok &= expect(context.capture_config.video.dark_threshold == 32.0F,
		             "capture callback receives runtime config");
		ok &= expect(context.capture_label == "front-door", "capture callback receives label");
		ok &= expect(context.append_calls == 1, "append callback called once");
		ok &= expect(context.appended_user == "alice", "append callback receives user");
		ok &=
		    expect(context.appended_entry.label == "front-door", "append callback receives label");
		ok &= expect(context.appended_entry.backend == howdy::native::FaceModel::kBackendName,
		             "append callback receives backend");
		ok &= expect(context.appended_entry.metric == "cosine", "append callback receives metric");
		ok &= expect(context.appended_entry.model == howdy::native::FaceModel::kSfaceModel,
		             "append callback receives model");
		ok &= expect(context.appended_entry.encodings.size() == 1,
		             "append callback receives one encoding vector");
		ok &= expect(context.appended_entry.encodings.front().size() == 1,
		             "append callback receives one encoding value");
		ok &= expect(context.appended_entry.encodings.front().front() == 0.125F,
		             "append callback receives encoding");
		ok &= expect(
		    (context.events == std::vector<std::string>{"load", "preflight", "capture", "append"}),
		    "successful add orders callbacks");
		return ok;
	}

	auto successful_interactive_flow_prompts_after_preflight() -> bool {
		auto               context = make_success_context();
		std::istringstream input("front-door\n");
		std::ostringstream output;
		context.input_stream = &input;
		StreamRedirect redirect(std::cin, input.rdbuf(), std::cout, output.rdbuf());

		const int result = run_add(context, {"howdy-add", "alice"});

		bool ok = true;
		ok &= expect(result == 0, "interactive successful add returns 0");
		ok &= expect(context.preflight_calls == 1, "interactive add calls preflight");
		ok &= expect(context.preflight_saw_unread_input,
		             "preflight runs before label input is consumed");
		ok &= expect(context.capture_calls == 1, "interactive add calls capture");
		ok &= expect(context.capture_label == "front-door", "capture receives entered label");
		ok &= expect(context.append_calls == 1, "interactive add calls append");
		ok &= expect(context.appended_entry.label == "front-door", "append receives entered label");
		ok &= expect(output.str().contains("Enter a label for this new model [automatic]: "),
		             "interactive add prompts for label");
		ok &= expect(
		    (context.events == std::vector<std::string>{"load", "preflight", "capture", "append"}),
		    "interactive add orders callbacks");
		return ok;
	}

	auto invalid_runtime_config_stops_before_preflight() -> bool {
		auto context          = make_success_context();
		context.config_result = invalid_config_load_result();

		const int result = run_add(context, {"howdy-add", "alice", "front-door"});

		bool ok = true;
		ok &= expect(result == 1, "invalid runtime config returns 1");
		ok &= expect(context.load_calls == 1, "runtime config callback called");
		ok &=
		    expect(context.preflight_calls == 0, "invalid runtime config skips preflight callback");
		ok &= expect(context.capture_calls == 0, "invalid runtime config skips capture callback");
		ok &= expect(context.append_calls == 0, "invalid runtime config skips append callback");
		return ok;
	}

	auto
	preflight_failure_stops_before_prompt(howdy::native::add_internal::AddPreflightStatus status,
	                                      const std::string &message, const std::string &test_name)
	    -> bool {
		auto context             = make_success_context();
		context.preflight_result = howdy::native::add_internal::AddPreflightResult{
		    .status        = status,
		    .error_message = message,
		};
		std::istringstream input("front-door\n");
		std::ostringstream output;
		context.input_stream = &input;
		StreamRedirect redirect(std::cin, input.rdbuf(), std::cout, output.rdbuf());

		const int result = run_add(context, {"howdy-add", "alice"});

		bool ok = true;
		ok &= expect(result == 1, test_name + " returns 1");
		ok &= expect(context.preflight_calls == 1, test_name + " calls preflight");
		ok &=
		    expect(context.preflight_saw_unread_input, test_name + " runs preflight before prompt");
		ok &= expect(context.capture_calls == 0, test_name + " skips capture");
		ok &= expect(context.append_calls == 0, test_name + " skips append");
		ok &= expect(input.tellg() == std::streampos(0), test_name + " leaves label input unread");
		ok &= expect(!output.str().contains("Enter a label for this new model"),
		             test_name + " does not prompt for label");
		return ok;
	}

	auto face_model_preflight_failure_stops_before_prompt() -> bool {
		return preflight_failure_stops_before_prompt(
		    howdy::native::add_internal::AddPreflightStatus::kFaceModelError, "face failed",
		    "face-model preflight failure");
	}

	auto incompatible_existing_model_stops_before_prompt() -> bool {
		return preflight_failure_stops_before_prompt(
		    howdy::native::add_internal::AddPreflightStatus::kExistingModelIncompatible, "",
		    "incompatible existing model");
	}

	auto existing_model_error_stops_before_prompt() -> bool {
		return preflight_failure_stops_before_prompt(
		    howdy::native::add_internal::AddPreflightStatus::kExistingModelError, "storage failed",
		    "existing model preflight error");
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

	auto expect_capture_failure_stops_before_append(const AddCliTestContext &context, int result,
	                                                const std::string &test_name) -> bool {
		bool ok = true;
		ok &= expect(result == 1, test_name + " returns 1");
		ok &= expect(context.preflight_calls == 1, test_name + " calls preflight callback once");
		ok &= expect(context.capture_calls == 1, test_name + " calls capture callback once");
		ok &= expect(context.append_calls == 0, test_name + " skips append callback");
		return ok;
	}

	auto black_frame_capture_failure_stops_before_append() -> bool {
		auto context           = make_success_context();
		context.capture_result = black_frame_capture_result();
		std::ostringstream error;
		ErrorRedirect      error_redirect(std::cerr, error.rdbuf());

		const int result = run_add(context, {"howdy-add", "alice", "front-door"});

		const auto error_output = error.str();
		bool       ok = expect_capture_failure_stops_before_append(context, result,
		                                                           "black-frame capture failure");
		ok &= expect(error_output.contains("Camera saw only black frames - is IR emitter working?"),
		             "black-frame capture failure prints IR emitter diagnostic");
		return ok;
	}

	auto only_too_dark_capture_failure_prints_diagnostic() -> bool {
		auto context           = make_success_context();
		context.capture_result = only_too_dark_capture_result();
		std::ostringstream error;
		ErrorRedirect      error_redirect(std::cerr, error.rdbuf());

		const int result = run_add(context, {"howdy-add", "alice", "front-door"});

		const auto error_output = error.str();
		bool       ok = expect_capture_failure_stops_before_append(context, result,
		                                                           "only-too-dark capture failure");
		ok &= expect(error_output.contains(
		                 "All frames were too dark, please check dark_threshold in config"),
		             "only-too-dark capture failure prints dark threshold diagnostic");
		ok &= expect(error_output.contains("Average darkness: 40, Threshold: 32"),
		             "only-too-dark capture failure prints average darkness and threshold");
		return ok;
	}

	auto no_sufficiently_bright_capture_failure_prints_diagnostic() -> bool {
		auto context           = make_success_context();
		context.capture_result = no_sufficiently_bright_capture_result();
		std::ostringstream error;
		ErrorRedirect      error_redirect(std::cerr, error.rdbuf());

		const int result = run_add(context, {"howdy-add", "alice", "front-door"});

		const auto error_output = error.str();
		bool       ok           = expect_capture_failure_stops_before_append(
		    context, result, "no-sufficiently-bright capture failure");
		ok &= expect(error_output.contains("No sufficiently bright frames captured, aborting"),
		             "no-sufficiently-bright capture failure prints diagnostic");
		ok &= expect(!error_output.contains(
		                 "All frames were too dark, please check dark_threshold in config"),
		             "no-sufficiently-bright capture failure does not print too-dark diagnostic");
		return ok;
	}

	auto no_usable_frames_capture_failure_prints_diagnostic() -> bool {
		auto context           = make_success_context();
		context.capture_result = no_usable_frames_capture_result();
		std::ostringstream error;
		ErrorRedirect      error_redirect(std::cerr, error.rdbuf());

		const int result = run_add(context, {"howdy-add", "alice", "front-door"});

		const auto error_output = error.str();
		bool ok = expect_capture_failure_stops_before_append(context, result,
		                                                     "no-usable-frames capture failure");
		ok &= expect(error_output.contains("No usable frames captured, aborting"),
		             "no-usable-frames capture failure prints diagnostic");
		return ok;
	}

	auto no_face_detected_capture_failure_prints_diagnostic() -> bool {
		auto context           = make_success_context();
		context.capture_result = no_face_detected_capture_result();
		std::ostringstream error;
		ErrorRedirect      error_redirect(std::cerr, error.rdbuf());

		const int result = run_add(context, {"howdy-add", "alice", "front-door"});

		const auto error_output = error.str();
		bool ok = expect_capture_failure_stops_before_append(context, result,
		                                                     "no-face-detected capture failure");
		ok &= expect(error_output.contains("No face detected, aborting"),
		             "no-face-detected capture failure prints diagnostic");
		return ok;
	}

	auto enrollment_failure_status_stops_before_append(
	    howdy::native::add_internal::AddEnrollmentStatus status, const std::string &test_name)
	    -> bool {
		auto context           = make_success_context();
		context.capture_result = enrollment_failure_result(status);

		const int result = run_add(context, {"howdy-add", "alice", "front-door"});

		bool ok = true;
		ok &= expect(result == 1, test_name + " returns 1");
		ok &= expect(context.preflight_calls == 1, test_name + " calls preflight callback");
		ok &= expect(context.capture_calls == 1, test_name + " calls capture callback");
		ok &= expect(context.append_calls == 0, test_name + " skips append");
		return ok;
	}

	auto face_model_enrollment_failure_stops_before_append() -> bool {
		return enrollment_failure_status_stops_before_append(
		    howdy::native::add_internal::AddEnrollmentStatus::kFaceModelError,
		    "face-model enrollment failure");
	}

	auto capture_open_failure_stops_before_append() -> bool {
		return enrollment_failure_status_stops_before_append(
		    howdy::native::add_internal::AddEnrollmentStatus::kCaptureOpenError,
		    "capture open failure");
	}

	auto multiple_faces_failure_stops_before_append() -> bool {
		return enrollment_failure_status_stops_before_append(
		    howdy::native::add_internal::AddEnrollmentStatus::kMultipleFaces,
		    "multiple-faces failure");
	}

	auto encoding_failure_stops_before_append() -> bool {
		auto context           = make_success_context();
		context.capture_result = howdy::native::add_internal::AddEnrollmentResult{
		    .status        = howdy::native::add_internal::AddEnrollmentStatus::kEncodingError,
		    .error_message = "SFace feature extraction failed: synthetic failure",
		};
		std::ostringstream error;
		ErrorRedirect      error_redirect(std::cerr, error.rdbuf());

		const int result = run_add(context, {"howdy-add", "alice", "front-door"});

		bool ok = true;
		ok &= expect(result == 1, "encoding failure returns 1");
		ok &= expect(context.append_calls == 0, "encoding failure skips append");
		ok &= expect(error.str().contains(context.capture_result.error_message),
		             "encoding failure prints actionable diagnostic");
		return ok;
	}

	auto unknown_enrollment_status_fails_closed_before_append() -> bool {
		auto context           = make_success_context();
		context.capture_result = howdy::native::add_internal::AddEnrollmentResult{
		    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
		    .status = static_cast<howdy::native::add_internal::AddEnrollmentStatus>(99),
		};
		std::ostringstream error;
		ErrorRedirect      error_redirect(std::cerr, error.rdbuf());

		const int result = run_add(context, {"howdy-add", "alice", "front-door"});

		bool ok = true;
		ok &= expect(result == 1, "unknown enrollment status returns 1");
		ok &= expect(context.preflight_calls == 1, "unknown enrollment status calls preflight");
		ok &= expect(context.capture_calls == 1, "unknown enrollment status calls capture");
		ok &= expect(context.append_calls == 0, "unknown enrollment status skips append");
		ok &= expect(error.str().contains("Internal error: unknown add enrollment status"),
		             "unknown enrollment status writes internal error");
		return ok;
	}

	auto plain_mode_skips_label_prompt() -> bool {
		auto               context = make_success_context();
		std::istringstream input("front-door\n");
		std::ostringstream output;
		context.input_stream = &input;
		StreamRedirect redirect(std::cin, input.rdbuf(), std::cout, output.rdbuf());

		const int result = run_add(context, {"howdy-add", "alice", "--plain"});

		bool ok = true;
		ok &= expect(result == 0, "plain add returns 0");
		ok &= expect(context.capture_plain, "plain flag reaches capture callback");
		ok &= expect(context.capture_label.empty(), "plain mode leaves automatic label empty");
		ok &= expect(context.appended_entry.label.empty(), "plain mode appends automatic label");
		ok &= expect(input.tellg() == std::streampos(0), "plain mode leaves label input unread");
		ok &= expect(!output.str().contains("Enter a label for this new model"),
		             "plain mode skips label prompt");
		return ok;
	}

	auto yes_flag_skips_label_prompt() -> bool {
		auto               context = make_success_context();
		std::istringstream input("front-door\n");
		std::ostringstream output;
		context.input_stream = &input;
		StreamRedirect redirect(std::cin, input.rdbuf(), std::cout, output.rdbuf());

		const int result = run_add(context, {"howdy-add", "alice", "-y"});

		bool ok = true;
		ok &= expect(result == 0, "yes add returns 0");
		ok &= expect(!context.capture_plain, "yes flag does not imply plain mode");
		ok &= expect(context.capture_label.empty(), "yes mode leaves automatic label empty");
		ok &= expect(context.appended_entry.label.empty(), "yes mode appends automatic label");
		ok &= expect(input.tellg() == std::streampos(0), "yes mode leaves label input unread");
		ok &= expect(!output.str().contains("Enter a label for this new model"),
		             "yes mode skips label prompt");
		return ok;
	}

	auto long_yes_argument_remains_label() -> bool {
		auto               context = make_success_context();
		std::istringstream input("front-door\n");
		std::ostringstream output;
		context.input_stream = &input;
		StreamRedirect redirect(std::cin, input.rdbuf(), std::cout, output.rdbuf());

		const int result = run_add(context, {"howdy-add", "alice", "--yes"});

		bool ok = true;
		ok &= expect(result == 0, "long yes argument add returns 0");
		ok &= expect(!context.capture_plain, "long yes argument does not imply plain mode");
		ok &= expect(context.capture_label == "--yes", "long yes argument remains label");
		ok &= expect(context.appended_entry.label == "--yes", "long yes argument appends as label");
		ok &= expect(input.tellg() == std::streampos(0),
		             "long yes argument leaves label input unread");
		ok &= expect(!output.str().contains("Enter a label for this new model"),
		             "long yes argument skips label prompt because it is label text");
		return ok;
	}

	auto command_label_removes_commas() -> bool {
		auto context = make_success_context();

		const int result = run_add(context, {"howdy-add", "alice", "front,door"});

		bool ok = true;
		ok &= expect(result == 0, "comma label add returns 0");
		ok &= expect(context.capture_label == "frontdoor", "capture receives comma-stripped label");
		ok &= expect(context.appended_entry.label == "frontdoor",
		             "append receives comma-stripped label");
		return ok;
	}

	auto interactive_label_truncates_to_24_characters() -> bool {
		auto               context = make_success_context();
		std::istringstream input("abcdefghijklmnopqrstuvwxyz\n");
		std::ostringstream output;
		StreamRedirect     redirect(std::cin, input.rdbuf(), std::cout, output.rdbuf());

		const int result = run_add(context, {"howdy-add", "alice"});

		bool ok = true;
		ok &= expect(result == 0, "long interactive label add returns 0");
		ok &= expect(context.capture_label == "abcdefghijklmnopqrstuvwx",
		             "capture receives truncated interactive label");
		ok &= expect(context.appended_entry.label == "abcdefghijklmnopqrstuvwx",
		             "append receives truncated interactive label");
		return ok;
	}

	auto append_failure_returns_error() -> bool {
		auto context          = make_success_context();
		context.append_result = howdy::native::UserModelMutationResult{
		    .status        = howdy::native::UserModelStatus::kWriteFailed,
		    .error_message = "append failed",
		};

		const int result = run_add(context, {"howdy-add", "alice", "front-door"});

		bool ok = true;
		ok &= expect(result == 1, "append failure returns 1");
		ok &= expect(context.preflight_calls == 1, "append failure calls preflight callback");
		ok &= expect(context.capture_calls == 1, "append failure calls capture callback");
		ok &= expect(context.append_calls == 1, "append failure calls append callback");
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
		ok &=
		    expect(context.preflight_calls == 0, "incomplete dependencies skip preflight callback");
		ok &= expect(context.capture_calls == 0,
		             "incomplete dependencies skip available capture callback");
		ok &= expect(context.append_calls == 0,
		             "incomplete dependencies skip available append callback");
		return ok;
	}

}  // namespace

auto main() -> int {
	bool ok = true;
	ok &= successful_enrollment_appends_expected_model();
	ok &= successful_interactive_flow_prompts_after_preflight();
	ok &= invalid_runtime_config_stops_before_preflight();
	ok &= face_model_preflight_failure_stops_before_prompt();
	ok &= incompatible_existing_model_stops_before_prompt();
	ok &= existing_model_error_stops_before_prompt();
	ok &= unknown_preflight_status_fails_closed_before_prompt();
	ok &= face_model_enrollment_failure_stops_before_append();
	ok &= capture_open_failure_stops_before_append();
	ok &= black_frame_capture_failure_stops_before_append();
	ok &= only_too_dark_capture_failure_prints_diagnostic();
	ok &= no_sufficiently_bright_capture_failure_prints_diagnostic();
	ok &= no_usable_frames_capture_failure_prints_diagnostic();
	ok &= no_face_detected_capture_failure_prints_diagnostic();
	ok &= multiple_faces_failure_stops_before_append();
	ok &= encoding_failure_stops_before_append();
	ok &= unknown_enrollment_status_fails_closed_before_append();
	ok &= plain_mode_skips_label_prompt();
	ok &= yes_flag_skips_label_prompt();
	ok &= long_yes_argument_remains_label();
	ok &= command_label_removes_commas();
	ok &= interactive_label_truncates_to_24_characters();
	ok &= append_failure_returns_error();
	ok &= incomplete_dependencies_stop_before_callbacks();
	return ok ? 0 : 1;
}
