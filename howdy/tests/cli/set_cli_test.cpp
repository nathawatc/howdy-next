#include "cli/set/internal.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>

namespace {

	using howdy::test::Expect;

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

	auto ResolveConfigPath(void *context) -> std::filesystem::path {
		auto &test_context = *static_cast<TestContext *>(context);
		++test_context.resolve_calls;
		return test_context.config_path;
	}

	auto UpdateConfigValue(void *context, const std::filesystem::path &config_path,
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

	auto DependenciesFor(TestContext &context) -> SetDependencies {
		return {
		    .context             = &context,
		    .resolve_config_path = ResolveConfigPath,
		    .update_config_value = UpdateConfigValue,
		};
	}

	auto RunSet(const howdy::native::CommandInvocation &invocation,
	            const SetDependencies                  &dependencies) -> RunResult {
		std::ostringstream output;
		auto              *previous_buffer = std::cout.rdbuf(output.rdbuf());
		const int          exit_code =
		    howdy::native::set_internal::SetMainWithDependencies(invocation, dependencies);
		std::cout.rdbuf(previous_buffer);
		return {.exit_code = exit_code, .output = output.str()};
	}

}  // namespace

auto main() -> int {
	constexpr auto unsafe_value_error =
	    "Config values must be single-line scalars and cannot start with [\n";
	bool ok = true;

	for (const auto &value : {std::string("line\nbreak"), std::string("[section]")}) {
		TestContext context;
		const auto  result = RunSet({.positionals = {"key", value}}, DependenciesFor(context));
		ok &= Expect(result.exit_code == 1, "unsafe scalar aborts");
		ok &= Expect(result.output == unsafe_value_error, "unsafe scalar prints error");
		ok &= Expect(context.resolve_calls == 1, "unsafe scalar resolves path first");
		ok &= Expect(context.update_calls == 0, "unsafe scalar skips updater");
	}

	{
		TestContext context;
		const auto  result = RunSet({.positionals = {"key", "-value"}}, DependenciesFor(context));
		ok &= Expect(result.exit_code == 0, "end-of-options value update succeeds");
		ok &= Expect(context.received_key == "key" && context.received_value == "-value",
		             "end-of-options preserves option-looking config value");
	}

	{
		TestContext context;
		const auto  result =
		    RunSet({.positionals = {"sface_threshold", "0.363"}}, DependenciesFor(context));
		ok &= Expect(result.exit_code == 0, "successful update succeeds");
		ok &= Expect(result.output == "Config option updated\n", "success output exact");
		ok &= Expect(context.resolve_calls == 1, "success calls resolver once");
		ok &= Expect(context.update_calls == 1, "success calls updater once");
		ok &= Expect(context.received_path == context.config_path, "updater receives path");
		ok &= Expect(context.received_key == "sface_threshold", "updater receives key");
		ok &= Expect(context.received_value == "0.363", "updater receives value");
		ok &= Expect(context.received_lock, "updater enables lock");
	}

	for (const auto &[error, expected_output] :
	     {std::pair{std::string("invalid config"), std::string("invalid config\n")},
	      std::pair{std::string(), std::string("Failed to update config option\n")}}) {
		TestContext context{
		    .update_result = false,
		    .update_error  = error,
		};
		const auto result = RunSet({.positionals = {"key", "value"}}, DependenciesFor(context));
		ok &= Expect(result.exit_code == 1, "updater failure aborts");
		ok &= Expect(result.output == expected_output, "updater failure output exact");
		ok &= Expect(!result.output.contains("Config option updated"),
		             "failure omits success output");
	}

	{
		TestContext context;
		auto        dependencies         = DependenciesFor(context);
		dependencies.resolve_config_path = nullptr;
		const auto result                = RunSet({.positionals = {"key", "value"}}, dependencies);
		ok &= Expect(result.exit_code == 1, "null resolver aborts");
		ok &= Expect(result.output.empty(), "null resolver prints nothing");
		ok &= Expect(context.resolve_calls == 0 && context.update_calls == 0,
		             "null resolver calls no dependencies");
	}

	{
		TestContext context;
		auto        dependencies         = DependenciesFor(context);
		dependencies.update_config_value = nullptr;
		const auto result                = RunSet({.positionals = {"key", "value"}}, dependencies);
		ok &= Expect(result.exit_code == 1, "null updater aborts");
		ok &= Expect(result.output.empty(), "null updater prints nothing");
		ok &= Expect(context.resolve_calls == 0 && context.update_calls == 0,
		             "null updater calls no dependencies");
	}

	return ok ? 0 : 1;
}
