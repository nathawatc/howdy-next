#include "compare/args.hpp"
#include "protocol/compare_exit.hpp"
#include "protocol/compare_invocation.hpp"
#include "support/user_names.hpp"
#include "test_support.hpp"

#include <string>
#include <vector>

namespace {

	using howdy::test::Expect;

	auto ArgvFrom(std::vector<std::string> &args) -> std::vector<char *> {
		std::vector<char *> result;
		result.reserve(args.size());
		for (auto &arg : args) {
			result.push_back(arg.data());
		}
		return result;
	}

}  // namespace

auto main() -> int {
	bool ok = true;

	{
		std::vector<std::string> args = {"howdy-compare", "alice"};
		auto                     argv = ArgvFrom(args);
		const auto result = howdy::native::ParseCompareArgs(static_cast<int>(argv.size()),
		                                                    argv.data(), "/tmp/config.toml");
		ok &= Expect(result.status == howdy::native::CompareArgsStatus::kOk,
		             "simple user parse succeeds");
		ok &= Expect(result.args.user == "alice", "user parsed");
		ok &=
		    Expect(result.args.config_path == "/tmp/config.toml", "default config path preserved");
	}

	{
		std::vector<std::string> args = {"howdy-compare", "--config", "/x.toml", "bob"};
		auto                     argv = ArgvFrom(args);
		const auto result = howdy::native::ParseCompareArgs(static_cast<int>(argv.size()),
		                                                    argv.data(), "/tmp/config.toml");
		ok &=
		    Expect(result.status == howdy::native::CompareArgsStatus::kOk, "config parse succeeds");
		ok &= Expect(result.args.user == "bob", "user parsed after config");
		ok &= Expect(result.args.config_path == "/x.toml", "custom config parsed");
	}

	{
		std::vector<std::string> args = {"howdy-compare", "alice", "bob"};
		auto                     argv = ArgvFrom(args);
		const auto result = howdy::native::ParseCompareArgs(static_cast<int>(argv.size()),
		                                                    argv.data(), "/tmp/config.toml");
		ok &= Expect(result.status == howdy::native::CompareArgsStatus::kError,
		             "surplus user is error");
		ok &= Expect(result.exit_code == howdy::native::CompareExit::kAbort, "surplus user aborts");
		ok &= Expect(result.message.contains("Unexpected argument: bob"),
		             "surplus user message populated");
	}

	{
		std::vector<std::string> args = {"howdy-compare", "--help"};
		auto                     argv = ArgvFrom(args);
		const auto result = howdy::native::ParseCompareArgs(static_cast<int>(argv.size()),
		                                                    argv.data(), "/tmp/config.toml");
		ok &= Expect(result.status == howdy::native::CompareArgsStatus::kHelp,
		             "help produces help status");
		ok &= Expect(result.exit_code == howdy::native::CompareExit::kSuccess,
		             "help returns success");
		ok &= Expect(result.message.contains("Usage: howdy-compare"), "help text populated");
	}

	{
		std::vector<std::string> args = {
		    "howdy-compare",
		    std::string(howdy::native::kCompareConfigOption),
		    "/x.toml",
		    "--help",
		};
		auto       argv   = ArgvFrom(args);
		const auto result = howdy::native::ParseCompareArgs(static_cast<int>(argv.size()),
		                                                    argv.data(), "/tmp/config.toml");
		ok &= Expect(result.status == howdy::native::CompareArgsStatus::kError,
		             "help in PAM username position is error");
		ok &= Expect(result.exit_code == howdy::native::CompareExit::kAbort,
		             "help in PAM username position aborts");
		ok &= Expect(result.message.contains("Unknown argument: --help"),
		             "help in PAM username position treated as unknown option");
	}

	{
		std::vector<std::string> args = {
		    "howdy-compare",
		    std::string(howdy::native::kCompareConfigOption),
		    "/x.toml",
		    "-h",
		};
		auto       argv   = ArgvFrom(args);
		const auto result = howdy::native::ParseCompareArgs(static_cast<int>(argv.size()),
		                                                    argv.data(), "/tmp/config.toml");
		ok &= Expect(result.status == howdy::native::CompareArgsStatus::kError,
		             "short help in PAM username position is error");
		ok &= Expect(result.exit_code == howdy::native::CompareExit::kAbort,
		             "short help in PAM username position aborts");
		ok &= Expect(result.message.contains("Unknown argument: -h"),
		             "short help in PAM username position treated as unknown option");
	}

	{
		std::vector<std::string> args = {"howdy-compare", "--bad"};
		auto                     argv = ArgvFrom(args);
		const auto result = howdy::native::ParseCompareArgs(static_cast<int>(argv.size()),
		                                                    argv.data(), "/tmp/config.toml");
		ok &= Expect(result.status == howdy::native::CompareArgsStatus::kError,
		             "unknown arg is error");
		ok &= Expect(result.exit_code == howdy::native::CompareExit::kAbort, "unknown arg aborts");
		ok &= Expect(result.message.contains("Unknown argument: --bad"),
		             "unknown arg message populated");
	}

	{
		std::vector<std::string> args = {"howdy-compare"};
		auto                     argv = ArgvFrom(args);
		const auto result = howdy::native::ParseCompareArgs(static_cast<int>(argv.size()),
		                                                    argv.data(), "/tmp/config.toml");
		ok &= Expect(result.status == howdy::native::CompareArgsStatus::kError,
		             "missing user is error");
		ok &= Expect(result.exit_code == howdy::native::CompareExit::kAbort, "missing user aborts");
	}

	{
		std::vector<std::string> args = {"howdy-compare", "../alice"};
		auto                     argv = ArgvFrom(args);
		const auto result = howdy::native::ParseCompareArgs(static_cast<int>(argv.size()),
		                                                    argv.data(), "/tmp/config.toml");
		ok &= Expect(result.status == howdy::native::CompareArgsStatus::kError,
		             "compare rejects path traversal username input");
		ok &= Expect(result.exit_code == howdy::native::CompareExit::kAbort,
		             "compare rejects malformed username input with abort");
	}

	{
		std::vector<std::string> args = {"howdy-compare", "alice..bob"};
		auto                     argv = ArgvFrom(args);
		const auto result = howdy::native::ParseCompareArgs(static_cast<int>(argv.size()),
		                                                    argv.data(), "/tmp/config.toml");
		ok &= Expect(result.status == howdy::native::CompareArgsStatus::kError,
		             "compare rejects malformed dot-dot username input");
		ok &= Expect(result.exit_code == howdy::native::CompareExit::kAbort,
		             "compare rejects malformed dot-dot username input with abort");
	}

	const auto expect_error = [&ok](std::vector<std::string> args, const std::string &message,
	                                const char *label) -> void {
		auto       argv   = ArgvFrom(args);
		const auto result = howdy::native::ParseCompareArgs(static_cast<int>(argv.size()),
		                                                    argv.data(), "/tmp/config.toml");
		ok &= Expect(
		    result.status == howdy::native::CompareArgsStatus::kError &&
		        result.exit_code == howdy::native::CompareExit::kAbort &&
		        (message.empty() ? result.message.empty() : result.message.contains(message)),
		    label);
	};
	expect_error({"howdy-compare", "--config"}, "Unknown argument: --config",
	             "trailing --config without value is unknown");
	expect_error({"howdy-compare", "--config", "/x.toml"}, "", "config without user is error");
	expect_error({"howdy-compare", ""}, "", "empty user is error");
	expect_error({"howdy-compare", "alice/bob"},
	             std::string(howdy::native::kInvalidUserNameMessage),
	             "slash in user name is rejected with message");
	expect_error({"howdy-compare", "../alice"}, std::string(howdy::native::kInvalidUserNameMessage),
	             "path traversal user reports invalid user message");

	return ok ? 0 : 1;
}
