#include "cli/set_internal.hpp"

#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

	using howdy::native::set_internal::SetDependencies;

	struct TestContext {
		std::filesystem::path config_path   = "/test/howdy/config.ini";
		int                   resolve_calls = 0;
		int                   update_calls  = 0;
		std::filesystem::path received_path;
		std::string           received_key;
		std::string           received_value;
		bool                  received_lock = false;
		bool                  update_result = true;
		std::string           update_error;
	};

	struct RunResult {
		int         exit_code;
		std::string output;
	};

	auto resolve_config_path(void *context) -> std::filesystem::path {
		auto &test_context = *static_cast<TestContext *>(context);
		++test_context.resolve_calls;
		return test_context.config_path;
	}

	auto update_config_value(void *context, const std::filesystem::path &config_path,
	                         const std::string &key, const std::string &value,
	                         std::string *error_message, bool lock) -> bool {
		auto &test_context = *static_cast<TestContext *>(context);
		++test_context.update_calls;
		test_context.received_path  = config_path;
		test_context.received_key   = key;
		test_context.received_value = value;
		test_context.received_lock  = lock;
		*error_message              = test_context.update_error;
		return test_context.update_result;
	}

	auto dependencies_for(TestContext &context) -> SetDependencies {
		return {
		    .context             = &context,
		    .resolve_config_path = resolve_config_path,
		    .update_config_value = update_config_value,
		};
	}

	auto run_set(std::vector<std::string> arguments, const SetDependencies &dependencies)
	    -> RunResult {
		std::vector<char *> argv;
		argv.reserve(arguments.size() + 1);
		for (auto &argument : arguments) {
			argv.push_back(argument.data());
		}
		argv.push_back(nullptr);

		std::ostringstream output;
		auto              *previous_buffer = std::cout.rdbuf(output.rdbuf());
		const int          exit_code = howdy::native::set_internal::set_main_with_dependencies(
		    static_cast<int>(arguments.size()), argv.data(), dependencies);
		std::cout.rdbuf(previous_buffer);
		return {.exit_code = exit_code, .output = output.str()};
	}

	auto expect(bool condition, const std::string &message) -> bool {
		if (!condition) {
			std::cerr << "FAIL: " << message << "\n";
			return false;
		}
		return true;
	}

}  // namespace

auto main() -> int {
	constexpr auto usage =
	    "Please add a setting you would like to change and the value to set it to\n"
	    "For example:\n"
	    "\n\thowdy set sface_threshold 0.363\n\n";
	constexpr auto unsafe_value_error =
	    "Config values must be single-line scalars and cannot start with [\n";
	bool ok = true;

	for (const auto &arguments :
	     {std::vector<std::string>{"howdy-set"}, std::vector<std::string>{"howdy-set", "key"}}) {
		TestContext context;
		const auto  result = run_set(arguments, dependencies_for(context));
		ok &= expect(result.exit_code == 1, "missing arguments abort");
		ok &= expect(result.output == usage, "missing arguments print usage");
		ok &= expect(context.resolve_calls == 0, "missing arguments skip resolver");
		ok &= expect(context.update_calls == 0, "missing arguments skip updater");
	}

	for (const auto &value : {std::string("line\nbreak"), std::string("[section]")}) {
		TestContext context;
		const auto  result = run_set({"howdy-set", "key", value}, dependencies_for(context));
		ok &= expect(result.exit_code == 1, "unsafe scalar aborts");
		ok &= expect(result.output == unsafe_value_error, "unsafe scalar prints error");
		ok &= expect(context.resolve_calls == 1, "unsafe scalar resolves path first");
		ok &= expect(context.update_calls == 0, "unsafe scalar skips updater");
	}

	{
		TestContext context;
		const auto  result =
		    run_set({"howdy-set", "sface_threshold", "0.363"}, dependencies_for(context));
		ok &= expect(result.exit_code == 0, "successful update succeeds");
		ok &= expect(result.output == "Config option updated\n", "success output exact");
		ok &= expect(context.resolve_calls == 1, "success calls resolver once");
		ok &= expect(context.update_calls == 1, "success calls updater once");
		ok &= expect(context.received_path == context.config_path, "updater receives path");
		ok &= expect(context.received_key == "sface_threshold", "updater receives key");
		ok &= expect(context.received_value == "0.363", "updater receives value");
		ok &= expect(context.received_lock, "updater enables lock");
	}

	for (const auto &[error, expected_output] :
	     {std::pair{std::string("invalid config"), std::string("invalid config\n")},
	      std::pair{std::string(), std::string("Failed to update config option\n")}}) {
		TestContext context{
		    .update_result = false,
		    .update_error  = error,
		};
		const auto result = run_set({"howdy-set", "key", "value"}, dependencies_for(context));
		ok &= expect(result.exit_code == 1, "updater failure aborts");
		ok &= expect(result.output == expected_output, "updater failure output exact");
		ok &= expect(!result.output.contains("Config option updated"),
		             "failure omits success output");
	}

	{
		TestContext context;
		const auto  result = run_set({"howdy-set", "sface_threshold", "0.363", "ignored"},
		                             dependencies_for(context));
		ok &= expect(result.exit_code == 0, "extra argument remains ignored");
		ok &= expect(context.received_key == "sface_threshold", "extra argument preserves key");
		ok &= expect(context.received_value == "0.363", "extra argument preserves value");
	}

	{
		TestContext context;
		auto        dependencies         = dependencies_for(context);
		dependencies.resolve_config_path = nullptr;
		const auto result                = run_set({"howdy-set", "key", "value"}, dependencies);
		ok &= expect(result.exit_code == 1, "null resolver aborts");
		ok &= expect(result.output.empty(), "null resolver prints nothing");
		ok &= expect(context.resolve_calls == 0 && context.update_calls == 0,
		             "null resolver calls no dependencies");
	}

	{
		TestContext context;
		auto        dependencies         = dependencies_for(context);
		dependencies.update_config_value = nullptr;
		const auto result                = run_set({"howdy-set", "key", "value"}, dependencies);
		ok &= expect(result.exit_code == 1, "null updater aborts");
		ok &= expect(result.output.empty(), "null updater prints nothing");
		ok &= expect(context.resolve_calls == 0 && context.update_calls == 0,
		             "null updater calls no dependencies");
	}

	return ok ? 0 : 1;
}
