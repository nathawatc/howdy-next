#include "cli/disable_cli.hpp"
#include "cli/disable_internal.hpp"
#include "test_support.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <sys/stat.h>

namespace {

	using howdy::test::expect;

	using howdy::native::RuntimeConfigLoadResult;
	using howdy::native::RuntimeConfigLoadStatus;
	using howdy::native::disable_internal::DisableDependencies;

	struct TestContext {
		int                     resolver_calls = 0;
		int                     loader_calls   = 0;
		int                     updater_calls  = 0;
		std::filesystem::path   resolved_path  = "/test/howdy/config.ini";
		std::filesystem::path   loader_path;
		std::filesystem::path   updater_path;
		std::string             updater_key;
		std::string             updater_value;
		bool                    received_lock             = false;
		bool                    received_validate_runtime = true;
		RuntimeConfigLoadResult load_result;
		bool                    updater_result = true;
		std::string             updater_error;
	};

	struct RunResult {
		int         exit_code;
		std::string stdout_output;
		std::string stderr_output;
	};

	auto loaded_config(bool disabled) -> RuntimeConfigLoadResult {
		howdy::native::RuntimeConfig config;
		config.core.disabled = disabled;
		return {
		    .ok     = true,
		    .status = RuntimeConfigLoadStatus::kOk,
		    .config = std::move(config),
		};
	}

	auto resolve_config_path(void *raw_context) -> std::filesystem::path {
		auto &context = *static_cast<TestContext *>(raw_context);
		++context.resolver_calls;
		return context.resolved_path;
	}

	auto load_runtime_config(void *raw_context, const std::filesystem::path &config_path)
	    -> RuntimeConfigLoadResult {
		auto &context = *static_cast<TestContext *>(raw_context);
		++context.loader_calls;
		context.loader_path = config_path;
		return context.load_result;
	}

	auto update_config_value(void *raw_context, const std::filesystem::path &config_path,
	                         const std::string &key, const std::string &value,
	                         std::string *error_message, bool lock, bool validate_runtime) -> bool {
		auto &context = *static_cast<TestContext *>(raw_context);
		++context.updater_calls;
		context.updater_path              = config_path;
		context.updater_key               = key;
		context.updater_value             = value;
		context.received_lock             = lock;
		context.received_validate_runtime = validate_runtime;
		*error_message                    = context.updater_error;
		return context.updater_result;
	}

	auto dependencies_for(TestContext &context) -> DisableDependencies {
		return {
		    .context             = &context,
		    .resolve_config_path = resolve_config_path,
		    .load_runtime_config = load_runtime_config,
		    .update_config_value = update_config_value,
		};
	}

	auto run_disable(std::vector<std::string> arguments, const DisableDependencies &dependencies)
	    -> RunResult {
		std::vector<char *> argv;
		argv.reserve(arguments.size() + 1);
		for (auto &argument : arguments) {
			argv.push_back(argument.data());
		}
		argv.push_back(nullptr);

		std::ostringstream stdout_stream;
		std::ostringstream stderr_stream;
		auto              *old_stdout = std::cout.rdbuf(stdout_stream.rdbuf());
		auto              *old_stderr = std::cerr.rdbuf(stderr_stream.rdbuf());
		const int exit_code = howdy::native::disable_internal::disable_main_with_dependencies(
		    static_cast<int>(arguments.size()), argv.data(), dependencies);
		std::cout.rdbuf(old_stdout);
		std::cerr.rdbuf(old_stderr);
		return {
		    .exit_code     = exit_code,
		    .stdout_output = stdout_stream.str(),
		    .stderr_output = stderr_stream.str(),
		};
	}

	auto expect_no_calls(const TestContext &context, const std::string &message) -> bool {
		return expect(context.resolver_calls == 0 && context.loader_calls == 0 &&
		                  context.updater_calls == 0,
		              message);
	}

	auto expect_update(const TestContext &context, const std::string &value,
	                   const std::string &message) -> bool {
		bool ok = true;
		ok &= expect(context.updater_calls == 1, message + " calls updater once");
		ok &= expect(context.updater_path == context.resolved_path, message + " passes path");
		ok &= expect(context.updater_key == "disabled", message + " passes key");
		ok &= expect(context.updater_value == value, message + " passes value");
		ok &= expect(context.received_lock, message + " enables lock");
		ok &= expect(!context.received_validate_runtime, message + " disables runtime validation");
		return ok;
	}

	auto write_file(const std::filesystem::path &path, const std::string &content) -> bool {
		std::ofstream output(path);
		output << content;
		return output.good();
	}

	auto read_file(const std::filesystem::path &path) -> std::string {
		std::ifstream input(path);
		return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	}

	auto run_public_disable(const std::string &argument) -> int {
		std::string           mutable_argument = argument;
		std::array<char *, 3> argv{const_cast<char *>("howdy-disable"), mutable_argument.data(),
		                           nullptr};
		return disable_main(2, argv.data());
	}

	auto public_entrypoint_integration() -> bool {
		namespace fs = std::filesystem;

		bool                             ok        = true;
		const fs::path                   temp_root = fs::current_path() / "howdy-disable-cli-test";
		const fs::path                   config_path = temp_root / "config.ini";
		const char                      *old_config  = std::getenv("HOWDY_CONFIG");
		const std::optional<std::string> saved_config =
		    old_config == nullptr ? std::nullopt : std::optional<std::string>{old_config};
		std::error_code error;

		fs::remove_all(temp_root, error);
		error.clear();
		fs::create_directories(temp_root, error);
		ok &= expect(!error, "integration creates temp directory");
		ok &= expect(chmod(temp_root.c_str(), 0755) == 0, "integration secures temp directory");
		ok &= expect(setenv("HOWDY_CONFIG", config_path.c_str(), 1) == 0,
		             "integration sets config path");
		ok &= expect(write_file(config_path, "[core]\ndisabled = false\n"),
		             "integration writes enabled config");
		ok &= expect(chmod(config_path.c_str(), 0644) == 0, "integration secures config file");
		ok &= expect(run_public_disable("true") == 0, "public disable succeeds");
		ok &=
		    expect(read_file(config_path).contains("disabled = true\n"), "public disable persists");
		ok &= expect(run_public_disable("false") == 0, "public enable succeeds");
		ok &=
		    expect(read_file(config_path).contains("disabled = false\n"), "public enable persists");

		const std::string invalid_runtime_config = "[video]\ntimeout = 0\n";
		ok &= expect(write_file(config_path, invalid_runtime_config),
		             "integration writes invalid runtime config");
		ok &= expect(run_public_disable("true") == 1,
		             "public disable rejects invalid runtime config");
		ok &= expect(read_file(config_path) == invalid_runtime_config,
		             "rejected invalid runtime config remains unchanged");

		const fs::path insecure_dir  = temp_root / "insecure-config-dir";
		const fs::path insecure_path = insecure_dir / "config.ini";
		fs::create_directories(insecure_dir, error);
		ok &= expect(!error, "integration creates insecure directory");
		ok &= expect(write_file(insecure_path, "[core]\ndisabled = false\n"),
		             "integration writes insecure config");
		ok &= expect(chmod(insecure_dir.c_str(), 0777) == 0,
		             "integration makes config directory insecure");
		ok &= expect(setenv("HOWDY_CONFIG", insecure_path.c_str(), 1) == 0,
		             "integration selects insecure config");
		ok &= expect(run_public_disable("true") == 1, "public disable rejects insecure directory");
		ok &= expect(read_file(insecure_path).contains("disabled = false\n"),
		             "rejected insecure config remains unchanged");

		if (saved_config.has_value()) {
			setenv("HOWDY_CONFIG", saved_config->c_str(), 1);
		} else {
			unsetenv("HOWDY_CONFIG");
		}
		chmod(insecure_dir.c_str(), 0755);
		fs::remove_all(temp_root, error);
		return ok;
	}

}  // namespace

auto main() -> int {
	bool ok = true;

	{
		TestContext context;
		const auto  result = run_disable({"howdy-disable"}, dependencies_for(context));
		ok &= expect(result.exit_code == 1, "missing argument returns 1");
		ok &= expect(result.stdout_output ==
		                 "Please add a 0 (enable) or a 1 (disable) as an argument\n",
		             "missing argument stdout exact");
		ok &= expect(result.stderr_output.empty(), "missing argument stderr empty");
		ok &= expect_no_calls(context, "missing argument skips dependencies");
	}

	{
		TestContext context;
		const auto  result = run_disable({"howdy-disable", "invalid"}, dependencies_for(context));
		ok &= expect(result.exit_code == 1, "invalid argument returns 1");
		ok &= expect(result.stdout_output ==
		                 "Please only use 0 (enable) or 1 (disable) as an argument\n",
		             "invalid argument stdout exact");
		ok &= expect(result.stderr_output.empty(), "invalid argument stderr empty");
		ok &= expect_no_calls(context, "invalid argument skips dependencies");
	}

	for (const auto &[argument, initially_disabled, value] :
	     {std::tuple{"1", false, "true"}, std::tuple{"true", false, "true"},
	      std::tuple{"0", true, "false"}, std::tuple{"false", true, "false"}}) {
		TestContext context;
		context.load_result = loaded_config(initially_disabled);
		const auto result   = run_disable({"howdy-disable", argument}, dependencies_for(context));
		ok &= expect(result.exit_code == 0, std::string(argument) + " alias succeeds");
		ok &= expect_update(context, value, std::string(argument) + " alias");
	}

	for (const int missing_callback : {0, 1, 2}) {
		TestContext context;
		auto        dependencies = dependencies_for(context);
		if (missing_callback == 0) {
			dependencies.resolve_config_path = nullptr;
		} else if (missing_callback == 1) {
			dependencies.load_runtime_config = nullptr;
		} else {
			dependencies.update_config_value = nullptr;
		}
		const auto result = run_disable({"howdy-disable", "1"}, dependencies);
		ok &= expect(result.exit_code == 1, "null dependency returns 1");
		ok &= expect(result.stdout_output.empty() && result.stderr_output.empty(),
		             "null dependency streams empty");
		ok &= expect_no_calls(context, "null dependency skips callbacks");
	}

	for (const auto &[status, error] :
	     {std::pair{RuntimeConfigLoadStatus::kPathError, "config path failed"},
	      std::pair{RuntimeConfigLoadStatus::kParseError, "config parse failed"},
	      std::pair{RuntimeConfigLoadStatus::kInvalidRuntimeValue, "config validation failed"},
	      std::pair{RuntimeConfigLoadStatus::kOk, "missing typed config"}}) {
		TestContext context;
		context.load_result = {.status = status, .error_message = error};
		const auto result   = run_disable({"howdy-disable", "1"}, dependencies_for(context));
		ok &= expect(result.exit_code == 1, "load failure returns 1");
		ok &= expect(result.stdout_output.empty(), "load failure stdout empty");
		ok &=
		    expect(result.stderr_output == std::string(error) + "\n", "load failure stderr exact");
		ok &= expect(context.resolver_calls == 1 && context.loader_calls == 1,
		             "load failure resolves and loads once");
		ok &= expect(context.loader_path == context.resolved_path, "loader receives resolved path");
		ok &= expect(context.updater_calls == 0, "load failure skips updater");
	}

	for (const auto &[argument, disabled, value] :
	     {std::tuple{"1", true, "true"}, std::tuple{"0", false, "false"}}) {
		TestContext context;
		context.load_result = loaded_config(disabled);
		const auto result   = run_disable({"howdy-disable", argument}, dependencies_for(context));
		ok &= expect(result.exit_code == 1, "unchanged state returns 1");
		ok &= expect(result.stdout_output ==
		                 std::string("The disable option has already been set to ") + value + "\n",
		             "unchanged state stdout exact");
		ok &= expect(result.stderr_output.empty(), "unchanged state stderr empty");
		ok &= expect(context.updater_calls == 0, "unchanged state skips updater");
	}

	for (const auto &[error, output] :
	     {std::pair{"atomic install failed", "atomic install failed\n"},
	      std::pair{"", "Failed to update \"disabled\" config option\n"}}) {
		TestContext context;
		context.load_result    = loaded_config(false);
		context.updater_result = false;
		context.updater_error  = error;
		const auto result      = run_disable({"howdy-disable", "true"}, dependencies_for(context));
		ok &= expect(result.exit_code == 1, "updater failure returns 1");
		ok &= expect(result.stdout_output == output, "updater failure stdout exact");
		ok &= expect(result.stderr_output.empty(), "updater failure stderr empty");
		ok &= expect(!result.stdout_output.contains("Howdy has been disabled"),
		             "updater failure omits success");
		ok &= expect_update(context, "true", "updater failure");
	}

	{
		TestContext context;
		context.load_result = loaded_config(false);
		const auto result   = run_disable({"howdy-disable", "true"}, dependencies_for(context));
		ok &= expect(result.exit_code == 0, "disable success returns 0");
		ok &= expect(result.stdout_output == "Howdy has been disabled\n",
		             "disable success stdout exact");
		ok &= expect(result.stderr_output.empty(), "disable success stderr empty");
		ok &= expect(context.resolver_calls == 1 && context.loader_calls == 1 &&
		                 context.updater_calls == 1,
		             "disable success calls each dependency once");
	}

	{
		TestContext context;
		context.load_result = loaded_config(true);
		const auto result   = run_disable({"howdy-disable", "false"}, dependencies_for(context));
		ok &= expect(result.exit_code == 0, "enable success returns 0");
		ok &= expect(result.stdout_output == "Howdy has been enabled\n",
		             "enable success stdout exact");
		ok &= expect(result.stderr_output.empty(), "enable success stderr empty");
		ok &= expect_update(context, "false", "enable success");
	}

	{
		TestContext context;
		context.load_result = loaded_config(false);
		const auto result =
		    run_disable({"howdy-disable", "true", "ignored"}, dependencies_for(context));
		ok &= expect(result.exit_code == 0, "extra argument remains ignored");
		ok &= expect_update(context, "true", "extra argument");
	}

	ok &= public_entrypoint_integration();

	return ok ? 0 : 1;
}
