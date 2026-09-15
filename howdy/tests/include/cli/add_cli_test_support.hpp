#pragma once

#include "cli/add/internal.hpp"
#include "test_support.hpp"

#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace howdy::test::add_cli {

	using howdy::test::Expect;

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

	inline auto ValidConfigLoadResult() -> howdy::native::RuntimeConfigLoadResult {
		howdy::native::RuntimeConfig config;
		config.video.dark_threshold = 32.0F;
		return howdy::native::RuntimeConfigLoadResult{
		    .ok     = true,
		    .status = howdy::native::RuntimeConfigLoadStatus::kOk,
		    .config = config,
		};
	}

	inline auto SuccessfulPreflightResult() -> howdy::native::add_internal::AddPreflightResult {
		return howdy::native::add_internal::AddPreflightResult{
		    .status = howdy::native::add_internal::AddPreflightStatus::kOk,
		};
	}

	inline auto SuccessfulCaptureResult() -> howdy::native::add_internal::AddEnrollmentResult {
		return howdy::native::add_internal::AddEnrollmentResult{
		    .status   = howdy::native::add_internal::AddEnrollmentStatus::kOk,
		    .metric   = howdy::native::FaceMetric::kCosine,
		    .encoding = {0.125F},
		};
	}

	inline auto LoadRuntimeConfigCallback(void *raw_context)
	    -> howdy::native::RuntimeConfigLoadResult {
		auto *context = static_cast<AddCliTestContext *>(raw_context);
		++context->load_calls;
		context->events.emplace_back("load");
		return context->config_result;
	}

	inline auto PreflightEnrollmentCallback(void *raw_context, const std::string &user,
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

	inline auto CaptureEnrollmentCallback(void *raw_context, const std::string &user,
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

	inline auto AppendUserModelEntryCallback(void *raw_context, const std::string &user,
	                                         const howdy::native::NewUserModelEntry &entry)
	    -> howdy::native::UserModelMutationResult {
		auto *context = static_cast<AddCliTestContext *>(raw_context);
		++context->append_calls;
		context->events.emplace_back("append");
		context->appended_user  = user;
		context->appended_entry = entry;
		return context->append_result;
	}

	inline auto TestDependencies(AddCliTestContext &context)
	    -> howdy::native::add_internal::AddDependencies {
		return howdy::native::add_internal::AddDependencies{
		    .context              = &context,
		    .load_runtime_config  = LoadRuntimeConfigCallback,
		    .preflight_enrollment = PreflightEnrollmentCallback,
		    .capture_enrollment   = CaptureEnrollmentCallback,
		    .append_user_model    = AppendUserModelEntryCallback,
		};
	}

	inline auto RunAddWithDependencies(howdy::native::add_internal::AddDependencies dependencies,
	                                   std::vector<std::string> arguments) -> int {
		std::vector<char *> argv;
		argv.reserve(arguments.size());
		for (auto &argument : arguments) {
			argv.push_back(argument.data());
		}
		return howdy::native::add_internal::AddMainWithDependencies(static_cast<int>(argv.size()),
		                                                            argv.data(), dependencies);
	}

	inline auto RunAdd(AddCliTestContext &context, std::vector<std::string> arguments) -> int {
		return RunAddWithDependencies(TestDependencies(context), std::move(arguments));
	}

	inline auto MakeSuccessContext() -> AddCliTestContext {
		AddCliTestContext context;
		context.config_result    = ValidConfigLoadResult();
		context.preflight_result = SuccessfulPreflightResult();
		context.capture_result   = SuccessfulCaptureResult();
		context.append_result    = howdy::native::UserModelMutationResult{
		    .status = howdy::native::UserModelStatus::kOk,
		};
		return context;
	}

	auto RunAddCliSuccessTests() -> bool;
	auto RunAddCliPreflightTests() -> bool;
	auto RunAddCliCaptureTests() -> bool;
	auto RunAddCliArgumentTests() -> bool;
	auto RunAddCliDependencyValidationTests() -> bool;

}  // namespace howdy::test::add_cli
