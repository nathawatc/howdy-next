#pragma once

#include "cli/add_internal.hpp"
#include "test_support.hpp"

#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace howdy::test::add_cli {

	using howdy::test::expect;

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

	inline auto valid_config_load_result() -> howdy::native::RuntimeConfigLoadResult {
		howdy::native::RuntimeConfig config;
		config.video.dark_threshold = 32.0F;
		return howdy::native::RuntimeConfigLoadResult{
		    .ok     = true,
		    .status = howdy::native::RuntimeConfigLoadStatus::kOk,
		    .config = config,
		};
	}

	inline auto successful_preflight_result() -> howdy::native::add_internal::AddPreflightResult {
		return howdy::native::add_internal::AddPreflightResult{
		    .status = howdy::native::add_internal::AddPreflightStatus::kOk,
		};
	}

	inline auto successful_capture_result() -> howdy::native::add_internal::AddEnrollmentResult {
		return howdy::native::add_internal::AddEnrollmentResult{
		    .status   = howdy::native::add_internal::AddEnrollmentStatus::kOk,
		    .metric   = "cosine",
		    .encoding = {0.125F},
		};
	}

	inline auto load_runtime_config_callback(void *raw_context)
	    -> howdy::native::RuntimeConfigLoadResult {
		auto *context = static_cast<AddCliTestContext *>(raw_context);
		++context->load_calls;
		context->events.emplace_back("load");
		return context->config_result;
	}

	inline auto preflight_enrollment_callback(void *raw_context, const std::string &user,
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

	inline auto capture_enrollment_callback(void *raw_context, const std::string &user,
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

	inline auto append_user_model_entry_callback(void *raw_context, const std::string &user,
	                                             const howdy::native::NewUserModelEntry &entry)
	    -> howdy::native::UserModelMutationResult {
		auto *context = static_cast<AddCliTestContext *>(raw_context);
		++context->append_calls;
		context->events.emplace_back("append");
		context->appended_user  = user;
		context->appended_entry = entry;
		return context->append_result;
	}

	inline auto test_dependencies(AddCliTestContext &context)
	    -> howdy::native::add_internal::AddDependencies {
		return howdy::native::add_internal::AddDependencies{
		    .context              = &context,
		    .load_runtime_config  = load_runtime_config_callback,
		    .preflight_enrollment = preflight_enrollment_callback,
		    .capture_enrollment   = capture_enrollment_callback,
		    .append_user_model    = append_user_model_entry_callback,
		};
	}

	inline auto run_add_with_dependencies(howdy::native::add_internal::AddDependencies dependencies,
	                                      std::vector<std::string> arguments) -> int {
		std::vector<char *> argv;
		argv.reserve(arguments.size());
		for (auto &argument : arguments) {
			argv.push_back(argument.data());
		}
		return howdy::native::add_internal::add_main_with_dependencies(
		    static_cast<int>(argv.size()), argv.data(), dependencies);
	}

	inline auto run_add(AddCliTestContext &context, std::vector<std::string> arguments) -> int {
		return run_add_with_dependencies(test_dependencies(context), std::move(arguments));
	}

	inline auto make_success_context() -> AddCliTestContext {
		AddCliTestContext context;
		context.config_result    = valid_config_load_result();
		context.preflight_result = successful_preflight_result();
		context.capture_result   = successful_capture_result();
		context.append_result    = howdy::native::UserModelMutationResult{
		    .status = howdy::native::UserModelStatus::kOk,
		};
		return context;
	}

	auto run_add_cli_success_tests() -> bool;
	auto run_add_cli_preflight_tests() -> bool;
	auto run_add_cli_capture_tests() -> bool;
	auto run_add_cli_argument_tests() -> bool;
	auto run_add_cli_dependency_validation_tests() -> bool;

}  // namespace howdy::test::add_cli
