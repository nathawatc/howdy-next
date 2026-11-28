#include "compare/args.hpp"
#include "protocol/compare_exit.hpp"
#include "support/user_names.hpp"
#include "test_support.hpp"

#include <array>
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

	auto ExpectProtocolModeRejections() -> bool {
		using howdy::native::CompareArgsMode;
		using howdy::native::CompareArgsStatus;
		using howdy::native::CompareExit;

		bool ok = true;

		// NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
		const auto invalid_mode = static_cast<CompareArgsMode>(255);
		for (const auto mode : {CompareArgsMode::kProtocol, invalid_mode}) {
			struct Case {
				std::vector<std::string> args;
				std::string              expected_message;
				const char              *label;
			};

			const std::vector<Case> cases = {
			    {
			        .args             = {"howdy-compare", "--help"},
			        .expected_message = "Unknown argument: --help",
			        .label            = "protocol mode rejects --help in PAM username position",
			    },
			    {
			        .args             = {"howdy-compare", "-h"},
			        .expected_message = "Unknown argument: -h",
			        .label            = "protocol mode rejects -h in PAM username position",
			    },
			    {
			        .args             = {"howdy-compare", "--"},
			        .expected_message = "Unknown argument: --",
			        .label            = "protocol mode rejects -- in PAM username position",
			    },
			    {
			        .args             = {"howdy-compare", "-"},
			        .expected_message = "Unknown argument: -",
			        .label            = "protocol mode rejects - in PAM username position",
			    },
			    {
			        .args             = {"howdy-compare", "--help", "alice"},
			        .expected_message = "Unknown argument: --help",
			        .label            = "protocol mode rejects --help before username",
			    },
			    {
			        .args             = {"howdy-compare", "alice", "--help"},
			        .expected_message = "Unknown argument: --help",
			        .label            = "protocol mode rejects trailing --help after username",
			    },
			    {
			        .args             = {"howdy-compare", "alice", "-h"},
			        .expected_message = "Unknown argument: -h",
			        .label            = "protocol mode rejects trailing -h after username",
			    },
			    {
			        .args             = {"howdy-compare", ""},
			        .expected_message = "",
			        .label            = "protocol mode rejects empty username",
			    },
			    {
			        .args             = {"howdy-compare", "../alice"},
			        .expected_message = std::string(howdy::native::kInvalidUserNameMessage),
			        .label            = "protocol mode rejects traversal username",
			    },
			};

			for (const auto &test_case : cases) {
				auto       args   = test_case.args;
				auto       argv   = ArgvFrom(args);
				const auto result = howdy::native::ParseCompareArgs(static_cast<int>(argv.size()),
				                                                    argv.data(), mode);
				const bool message_ok =
				    test_case.expected_message.empty()
				        ? result.message.empty()
				        : (result.message.contains(test_case.expected_message) &&
				           result.message.ends_with('\n'));
				ok &= Expect(result.status == CompareArgsStatus::kError &&
				                 result.exit_code == CompareExit::kAbort && message_ok,
				             test_case.label);
			}
		}

		std::vector<std::string> valid_args   = {"howdy-compare", "alice"};
		auto                     valid_argv   = ArgvFrom(valid_args);
		const auto               valid_result = howdy::native::ParseCompareArgs(
		    static_cast<int>(valid_argv.size()), valid_argv.data(), CompareArgsMode::kProtocol);
		ok &= Expect(valid_result.status == CompareArgsStatus::kOk &&
		                 valid_result.exit_code == CompareExit::kSuccess &&
		                 valid_result.args.user == "alice",
		             "protocol mode accepts valid username");

		return ok;
	}

}  // namespace

auto main() -> int {
	bool ok = true;

	{
		std::vector<std::string> args = {"howdy-compare", "alice"};
		auto                     argv = ArgvFrom(args);
		const auto               result =
		    howdy::native::ParseCompareArgs(static_cast<int>(argv.size()), argv.data());
		ok &= Expect(result.status == howdy::native::CompareArgsStatus::kOk,
		             "simple user parse succeeds");
		ok &= Expect(result.args.user == "alice", "user parsed");
	}

	{
		std::vector<std::string> args = {"howdy-compare", "alice", "bob"};
		auto                     argv = ArgvFrom(args);
		const auto               result =
		    howdy::native::ParseCompareArgs(static_cast<int>(argv.size()), argv.data());
		ok &= Expect(result.status == howdy::native::CompareArgsStatus::kError,
		             "surplus user is error");
		ok &= Expect(result.exit_code == howdy::native::CompareExit::kAbort, "surplus user aborts");
		ok &= Expect(result.message.contains("Unexpected argument: bob"),
		             "surplus user message populated");
	}

	for (const char *help_flag : {"--help", "-h"}) {
		std::vector<std::string> args = {"howdy-compare", help_flag};
		auto                     argv = ArgvFrom(args);
		const auto               result =
		    howdy::native::ParseCompareArgs(static_cast<int>(argv.size()), argv.data());
		ok &= Expect(result.status == howdy::native::CompareArgsStatus::kHelp,
		             "standalone help produces help status");
		ok &= Expect(result.exit_code == howdy::native::CompareExit::kSuccess,
		             "standalone help returns success");
		ok &= Expect(result.message.contains("Usage: howdy-compare"),
		             "standalone help text populated");
	}

	{
		std::vector<std::string> args = {
		    "howdy-compare",
		    "alice",
		    "--help",
		};
		auto       argv = ArgvFrom(args);
		const auto result =
		    howdy::native::ParseCompareArgs(static_cast<int>(argv.size()), argv.data());
		ok &= Expect(result.status == howdy::native::CompareArgsStatus::kError,
		             "trailing help option after username is error");
		ok &= Expect(result.exit_code == howdy::native::CompareExit::kAbort,
		             "trailing help option after username aborts");
		ok &= Expect(result.message.contains("Unknown argument: --help"),
		             "trailing help option after username treated as unknown option");
	}

	{
		std::vector<std::string> args = {
		    "howdy-compare",
		    "alice",
		    "-h",
		};
		auto       argv = ArgvFrom(args);
		const auto result =
		    howdy::native::ParseCompareArgs(static_cast<int>(argv.size()), argv.data());
		ok &= Expect(result.status == howdy::native::CompareArgsStatus::kError,
		             "trailing short help option after username is error");
		ok &= Expect(result.exit_code == howdy::native::CompareExit::kAbort,
		             "trailing short help option after username aborts");
		ok &= Expect(result.message.contains("Unknown argument: -h"),
		             "trailing short help option after username treated as unknown option");
	}

	ok &= ExpectProtocolModeRejections();

	{
		std::vector<std::string> args = {"howdy-compare", "--bad"};
		auto                     argv = ArgvFrom(args);
		const auto               result =
		    howdy::native::ParseCompareArgs(static_cast<int>(argv.size()), argv.data());
		ok &= Expect(result.status == howdy::native::CompareArgsStatus::kError,
		             "unknown arg is error");
		ok &= Expect(result.exit_code == howdy::native::CompareExit::kAbort, "unknown arg aborts");
		ok &= Expect(result.message.contains("Unknown argument: --bad"),
		             "unknown arg message populated");
	}

	{
		std::vector<std::string> args = {"howdy-compare"};
		auto                     argv = ArgvFrom(args);
		const auto               result =
		    howdy::native::ParseCompareArgs(static_cast<int>(argv.size()), argv.data());
		ok &= Expect(result.status == howdy::native::CompareArgsStatus::kError,
		             "missing user is error");
		ok &= Expect(result.exit_code == howdy::native::CompareExit::kAbort, "missing user aborts");
	}

	{
		std::vector<std::string> args = {"howdy-compare", "../alice"};
		auto                     argv = ArgvFrom(args);
		const auto               result =
		    howdy::native::ParseCompareArgs(static_cast<int>(argv.size()), argv.data());
		ok &= Expect(result.status == howdy::native::CompareArgsStatus::kError,
		             "compare rejects path traversal username input");
		ok &= Expect(result.exit_code == howdy::native::CompareExit::kAbort,
		             "compare rejects malformed username input with abort");
	}

	{
		std::vector<std::string> args = {"howdy-compare", "alice..bob"};
		auto                     argv = ArgvFrom(args);
		const auto               result =
		    howdy::native::ParseCompareArgs(static_cast<int>(argv.size()), argv.data());
		ok &= Expect(result.status == howdy::native::CompareArgsStatus::kError,
		             "compare rejects malformed dot-dot username input");
		ok &= Expect(result.exit_code == howdy::native::CompareExit::kAbort,
		             "compare rejects malformed dot-dot username input with abort");
	}

	const auto expect_error = [&ok](std::vector<std::string> args, const std::string &message,
	                                const char *label) -> void {
		auto       argv = ArgvFrom(args);
		const auto result =
		    howdy::native::ParseCompareArgs(static_cast<int>(argv.size()), argv.data());
		ok &= Expect(result.status == howdy::native::CompareArgsStatus::kError &&
		                 result.exit_code == howdy::native::CompareExit::kAbort &&
		                 (message.empty() ? result.message.empty()
		                                  : (result.message.contains(message) &&
		                                     result.message.ends_with('\n'))),
		             label);
	};
	expect_error({"howdy-compare", "--config", "/x.ini", "alice"}, "Unknown argument: --config",
	             "--config is no longer accepted");
	expect_error({"howdy-compare", ""}, "", "empty user is error");
	expect_error({"howdy-compare", "alice/bob"},
	             std::string(howdy::native::kInvalidUserNameMessage),
	             "slash in user name is rejected with message");
	expect_error({"howdy-compare", "../alice"}, std::string(howdy::native::kInvalidUserNameMessage),
	             "path traversal user reports invalid user message");

	{
		for (const auto bad_argc : {0, -1, 2}) {
			const auto result = howdy::native::ParseCompareArgs(bad_argc, nullptr);
			ok &= Expect(result.status == howdy::native::CompareArgsStatus::kError &&
			                 result.exit_code == howdy::native::CompareExit::kAbort,
			             "null argv fails closed");
		}
		std::string           help_flag    = "--help";
		std::array<char *, 2> null_program = {nullptr, help_flag.data()};
		const auto            null_argv0   = howdy::native::ParseCompareArgs(
		    static_cast<int>(null_program.size()), null_program.data());
		ok &= Expect(null_argv0.status == howdy::native::CompareArgsStatus::kError &&
		                 null_argv0.exit_code == howdy::native::CompareExit::kAbort,
		             "null argv[0] fails closed");

		std::string           prog_name     = "howdy-compare";
		std::array<char *, 2> null_user_arg = {prog_name.data(), nullptr};
		const auto            null_argv1    = howdy::native::ParseCompareArgs(
		    static_cast<int>(null_user_arg.size()), null_user_arg.data());
		ok &= Expect(null_argv1.status == howdy::native::CompareArgsStatus::kError &&
		                 null_argv1.exit_code == howdy::native::CompareExit::kAbort,
		             "null argv[1] fails closed");
	}

	return ok ? 0 : 1;
}
