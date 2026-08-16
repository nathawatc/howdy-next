#include "app/command_catalog.hpp"
#include "app/howdy_dispatch_test_support.hpp"
#include "app/howdy_internal.hpp"
#include "config/config_schema.hpp"
#include "howdy/version_format.hpp"
#include "test_support.hpp"
#include "version.hpp"

#include <array>
#include <string>
#include <vector>

namespace howdy::test::dispatch {

	namespace {

		using howdy::test::expect;

		using howdy::native::command_catalog;
		using howdy::native::CommandId;
		using howdy::native::GlobalOptionDescriptor;
		using howdy::native::GlobalOptionId;
		using howdy::native::howdy_internal::CommandMain;
		using howdy::native::howdy_internal::format_usage_options;

		auto expected_runtime_config_keys() -> std::string {
			std::string expected;
			for (const auto &option : howdy::native::config_schema::runtime_config_options()) {
				expected += option.key;
				expected += '\n';
			}
			return expected;
		}

		auto expected_config_option_values(const howdy::native::config_schema::Option &option)
		    -> std::string {
			std::string expected;
			if (!option.choices.empty()) {
				for (const auto choice : option.choices) {
					expected += choice;
					expected += '\n';
				}
				return expected;
			}
			if (option.type == howdy::native::config_schema::ValueType::boolean) {
				return "false\ntrue\n";
			}
			return {};
		}

		auto expected_global_option_completion() -> std::string {
			std::string expected;
			for (const auto &option : howdy::native::global_option_catalog()) {
				for (const auto spelling : {option.short_name, option.long_name}) {
					if (!spelling.empty()) {
						expected += spelling;
						expected += '\t';
						expected += option.argument_name.empty() ? '0' : '1';
						expected += '\t';
						expected += option.parses_after_command ? '1' : '0';
						expected += '\t';
						expected +=
						    option.completion == howdy::native::GlobalOptionCompletionKind::kUser
						        ? "user"
						        : "none";
						expected += '\n';
					}
				}
			}
			return expected;
		}

		auto test_usage_option_rendering() -> bool {
			const std::array options{
			    GlobalOptionDescriptor{
			        .id            = GlobalOptionId::kUser,
			        .short_name    = "-u",
			        .long_name     = "--user-test",
			        .argument_name = "USER",
			    },
			    GlobalOptionDescriptor{
			        .id        = GlobalOptionId::kPlain,
			        .long_name = "--plain-test",
			        .summary   = "Plain",
			    },
			    GlobalOptionDescriptor{
			        .id         = GlobalOptionId::kYes,
			        .short_name = "-y-test",
			        .summary    = "Yes",
			    },
			};
			return expect(format_usage_options(options) == "[-u USER] [--plain-test] [-y-test]",
			              "usage renderer includes every descriptor with generic spellings");
		}

		auto test_completion_metadata_behavior() -> bool {
			bool ok = true;
			{
				Context    context;
				const auto result = run(context, {"howdy", "__complete", "global-options"});
				ok &= expect(
				    result.status == 0 && result.output == expected_global_option_completion() &&
				        result.error.empty(),
				    "global option completion query returns catalog aliases and argument metadata");
				ok &= expect(context.resolve_user_calls == 0 && context.effective_uid_calls == 0 &&
				                 !context.command_id.has_value(),
				             "global option completion query skips normal dispatch flow");
			}
			{
				Context    context;
				const auto result =
				    run(context, {"howdy", "__complete", "command-options", "test"});
				ok &=
				    expect(result.status == 0 &&
				               result.output == "-U\t1\tnone\n--user\t1\tnone\n--device\t1\tnone\n",
				           "test completion exposes applicable global and command options");
				ok &= expect(context.resolve_user_calls == 0 && context.effective_uid_calls == 0 &&
				                 !context.command_id.has_value(),
				             "command option completion skips normal dispatch flow");
			}
			{
				Context    context;
				const auto result =
				    run(context, {"howdy", "__complete", "command-options", "disable"});
				ok &= expect(result.status == 0 && result.output.empty(),
				             "inapplicable command options are not advertised");
			}
			{
				Context    context;
				const auto result =
				    run(context, {"howdy", "__complete", "command-values", "disable", "0"});
				ok &= expect(result.status == 0 && result.output == "false\ntrue\n",
				             "boolean command completion comes from command metadata");
				ok &= expect(context.resolve_user_calls == 0 && context.effective_uid_calls == 0 &&
				                 !context.command_id.has_value(),
				             "command value completion skips normal dispatch flow");
			}
			{
				Context    context;
				const auto result =
				    run(context, {"howdy", "__complete", "command-values", "set", "0"});
				ok &= expect(result.status == 0 && result.output == expected_runtime_config_keys(),
				             "set key completion follows runtime config schema order");
				ok &= expect(context.resolve_user_calls == 0 && context.effective_uid_calls == 0 &&
				                 !context.command_id.has_value(),
				             "set key completion skips normal dispatch flow");
			}
			for (const auto &option : howdy::native::config_schema::runtime_config_options()) {
				Context    context;
				const auto result = run(context, {"howdy", "__complete", "command-values", "set",
				                                  "1", std::string(option.key)});
				ok &= expect(
				    result.status == 0 && result.output == expected_config_option_values(option),
				    "set value completion follows schema metadata for " + std::string(option.key));
			}
			{
				Context    context;
				const auto result = run(
				    context, {"howdy", "__complete", "command-values", "set", "1", "unknown_key"});
				ok &= expect(result.status == 0 && result.output.empty(),
				             "unknown set key has no completion candidates");
			}
			{
				Context    context;
				const auto result =
				    run(context, {"howdy", "__complete", "command-values", "set", "1"});
				ok &= expect(result.status != 0 && result.output.empty(),
				             "set value completion without key is malformed");
			}
			{
				Context    context;
				const auto result = run(context, {"howdy", "__complete", "command-values", "set",
				                                  "1", "timeout", "extra"});
				ok &= expect(result.status != 0 && result.output.empty(),
				             "set value completion with malformed context is rejected");
			}
			{
				Context    context;
				const auto result =
				    run(context, {"howdy", "__complete", "command-values", "version", "0"});
				ok &= expect(result.status == 0 && result.output.empty(),
				             "unrelated command has no positional completion values");
				ok &= expect(context.resolve_user_calls == 0 && context.effective_uid_calls == 0 &&
				                 !context.command_id.has_value(),
				             "empty command value completion skips normal dispatch flow");
			}
			const std::vector<std::vector<std::string>> invalid_queries = {
			    {"howdy", "__complete", "command-values", "unknown", "0"},
			    {"howdy", "__complete", "command-values", "disable", "bad"},
			    {"howdy", "__complete", "command-values", "disable", "1"},
			};
			for (const auto &arguments : invalid_queries) {
				Context    context;
				const auto result = run(context, arguments);
				ok &= expect(result.status != 0 && result.output.empty(),
				             "invalid command completion value query is rejected cleanly");
			}
			return ok;
		}

		auto test_completion_behavior() -> bool {
			bool ok = true;
			ok &= test_usage_option_rendering();
			ok &= test_completion_metadata_behavior();

			{
				Context    context;
				const auto result = run(context, {"howdy", "--help"});
				ok &= expect(result.status == 0 && result.output.contains("commands:"),
				             "help succeeds");
				ok &= expect(
				    result.output.starts_with(
				        "usage: howdy [-U USER] [--plain] [-h] [-y] {command} [arguments...]\n"),
				    "help usage ordering is preserved");
				const auto usage_end  = result.output.find('\n');
				const auto usage_line = result.output.substr(0, usage_end);
				for (const auto &option : howdy::native::global_option_catalog()) {
					std::string expected = option.short_name.empty()
					                           ? std::string(option.long_name)
					                           : std::string(option.short_name);
					if (!option.argument_name.empty()) {
						expected += ' ';
						expected += option.argument_name;
					}
					expected.insert(0, 1, '[');
					expected += ']';
					ok &= expect(usage_line.contains(expected),
					             "usage includes every catalog global option");
				}
				for (const auto &command : command_catalog()) {
					ok &= expect(result.output.contains(command.name),
					             "help lists every catalog command");
					ok &= expect(result.output.contains(command.summary),
					             "help uses every catalog summary");
				}
				for (const auto &option : howdy::native::global_option_catalog()) {
					ok &= expect(result.output.contains(option.summary),
					             "help uses every catalog option summary");
				}
				ok &= expect(!result.output.contains("__complete"),
				             "completion query stays out of help");
			}
			{
				Context     context;
				const auto  result = run(context, {"howdy", "__complete", "commands"});
				std::string expected;
				for (const auto &command : command_catalog()) {
					expected += command.name;
					expected += '\n';
				}
				ok &=
				    expect(result.status == 0 && result.output == expected && result.error.empty(),
				           "completion query returns canonical catalog command list");
				ok &= expect(context.resolve_user_calls == 0 && context.effective_uid_calls == 0,
				             "completion query skips user and root checks");
				ok &= expect(!context.command_id.has_value() && context.command_arguments.empty(),
				             "completion query does not dispatch a command");
			}
			{
				Context    context;
				const auto result = run(context, {"howdy", "__complete", "commands", "--", "-y"});
				ok &= expect(result.status == 0 && result.output == "add\nclear\nremove\n",
				             "command completion filters by applicable yes option");
			}
			{
				Context    context;
				const auto result =
				    run(context, {"howdy", "__complete", "commands", "--", "--plain"});
				ok &= expect(result.status == 0 && result.output == "add\nlist\n",
				             "command completion filters by applicable plain option");
			}
			{
				Context    context;
				const auto result = run(context, {"howdy", "__complete", "commands", "--", "-U"});
				ok &= expect(result.status == 0 &&
				                 result.output == "add\nclear\nlist\nremove\ntest\n",
				             "command completion filters by applicable user option");
			}
			{
				const std::vector<std::vector<std::string>> malformed_queries = {
				    {"howdy", "__complete"},
				    {"howdy", "__complete", "unknown"},
				    {"howdy", "__complete", "commands", "extra"},
				    {"howdy", "__complete", "commands", "-y"},
				    {"howdy", "-y", "__complete", "commands"},
				    {"howdy", "__complete", "--plain", "commands"},
				    {"howdy", "__complete", "commands", "--plain"},
				    {"howdy", "__complete", "global-options", "extra"},
				    {"howdy", "__complete", "command-values", "disable"},
				    {"howdy", "__complete", "commands", "-U", "bob"},
				    {"howdy", "-U", "bob", "__complete", "commands"},
				    {"howdy", "--user", "bob", "__complete", "commands"},
				};
				for (const auto &arguments : malformed_queries) {
					Context    context;
					const auto result = run(context, arguments);
					ok &= expect(result.status != 0, "malformed completion query is rejected");
					ok &= expect(!result.output.contains("add\n"),
					             "malformed completion query prints no command list");
					ok &=
					    expect(context.resolve_user_calls == 0 && context.effective_uid_calls == 0,
					           "malformed completion query skips user and root checks");
					ok &=
					    expect(!context.command_id.has_value() && context.command_arguments.empty(),
					           "malformed completion query does not dispatch a command");
				}
			}
			{
				Context    context;
				const auto result = run(context, {"howdy", "config", "--help"});
				ok &= expect(result.status != 0 && !context.command_id.has_value(),
				             "help after command is rejected by strict command syntax");
			}
			{
				ok &= expect(
				    howdy::native::format_version(howdy::native::kProjectVersion, "abcdef1234") ==
				        "Howdy Next " + std::string(howdy::native::kProjectVersion) +
				            " (abcdef1234)",
				    "version formatter includes ten-character commit");
				ok &= expect(howdy::native::format_version(howdy::native::kProjectVersion, "") ==
				                 "Howdy Next " + std::string(howdy::native::kProjectVersion),
				             "version formatter omits empty commit");
				Context    context;
				const auto result =
				    run(context, {"howdy", "version"}, nullptr,
				        std::array<CommandMain, static_cast<std::size_t>(CommandId::kCount)>{});
				const auto expected = howdy::native::format_version(howdy::native::kProjectVersion,
				                                                    howdy::native::kBuildCommit) +
				                      "\n";
				ok &= expect(result.status == 0 && result.output == expected,
				             "version output is formatted");
				ok &= expect(context.resolve_user_calls == 0 && context.effective_uid_calls == 0,
				             "version skips user and root checks");
				ok &= expect(!context.command_id.has_value() && context.command_arguments.empty(),
				             "version does not dispatch a command");
			}

			return ok;
		}

	}  // namespace

	auto run_howdy_completion_tests() -> bool {
		return test_completion_behavior();
	}

}  // namespace howdy::test::dispatch
