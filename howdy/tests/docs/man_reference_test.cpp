#include "app/command_catalog.hpp"
#include "docs/man_reference.hpp"
#include "module/pam_option_catalog.hpp"
#include "test_support.hpp"

#include <array>
#include <string>
#include <string_view>

namespace {

	using howdy::docs::render_command_reference;
	using howdy::docs::render_global_option_reference;
	using howdy::docs::render_workaround_reference;
	using howdy::native::CommandDescriptor;
	using howdy::native::CommandId;
	using howdy::native::GlobalOptionDescriptor;
	using howdy::pam::Workaround;
	using howdy::pam::WorkaroundDescriptor;
	using howdy::test::expect;

	auto has_exactly_one_final_newline(std::string_view value) -> bool {
		return !value.empty() && value.back() == '\n' &&
		       (value.size() == 1 || value[value.size() - 2] != '\n');
	}

}  // namespace

auto main() -> int {
	bool ok = true;

	const auto command_result = render_command_reference(howdy::native::command_catalog());
	ok &= expect(command_result.ok(), "production commands render");
	ok &= expect(has_exactly_one_final_newline(command_result.output),
	             "command reference has one final newline");
	ok &= expect(command_result.output.contains(R"(\&\fBadd\fR)"),
	             "command reference renders command name");
	ok &= expect(command_result.output.contains("Add face model"),
	             "command reference renders command summary");
	const auto misleading_phrase = std::string{"target user"} + " required";
	ok &= expect(!command_result.output.contains(misleading_phrase),
	             "command reference does not imply explicit user input is required");
	ok &= expect(command_result.output.contains(R"(\&\fBversion\fR)"),
	             "command reference renders version command");

	const auto option_result =
	    render_global_option_reference(howdy::native::global_option_catalog());
	ok &= expect(option_result.ok(), "production global options render");
	ok &= expect(has_exactly_one_final_newline(option_result.output),
	             "option reference has one final newline");
	ok &= expect(option_result.output.contains(R"(\-U, \-\-user USER)"),
	             "option reference renders short and long spelling");
	ok &= expect(option_result.output.contains(R"(\-y)"),
	             "option reference renders short-only spelling");
	ok &= expect(option_result.output.contains(R"(\-\-plain)"),
	             "option reference renders long-only spelling");

	const auto workaround_result = render_workaround_reference(howdy::pam::workaround_catalog(),
	                                                           howdy::pam::kDefaultWorkaround);
	ok &= expect(workaround_result.ok(), "production workaround values render");
	ok &= expect(has_exactly_one_final_newline(workaround_result.output),
	             "workaround reference has one final newline");
	ok &= expect(workaround_result.output.contains("workaround=input"),
	             "workaround input value renders");
	ok &= expect(workaround_result.output.contains("workaround=native"),
	             "workaround native value renders");
	ok &= expect(workaround_result.output.contains(R"(workaround=native\-input)"),
	             "workaround native-input value renders");
	ok &=
	    expect(workaround_result.output.contains("Workaround is off when workaround= is omitted."),
	           "workaround default is documented");
	ok &= expect(!workaround_result.output.contains("workaround=off"),
	             "invalid off token is not advertised as mapped value");

	const auto repeat_command_result = render_command_reference(howdy::native::command_catalog());
	ok &= expect(command_result.output == repeat_command_result.output,
	             "command rendering is deterministic");

	const std::array escaped_commands{
	    CommandDescriptor{
	        .id          = CommandId::kAdd,
	        .name        = "a-b\\c",
	        .summary     = "A-b\\c",
	        .kind        = howdy::native::CommandKind::kEntrypoint,
	        .user_target = howdy::native::UserTargetMode::kNone,
	    },
	};
	const auto escaped_result = render_command_reference(escaped_commands);
	ok &= expect(escaped_result.ok(), "safe roff metadata renders");
	ok &= expect(escaped_result.output.contains(R"(a\-b\\c)"),
	             "roff escapes hyphens and backslashes");

	const std::array duplicate_command_ids{
	    CommandDescriptor{
	        .id          = CommandId::kAdd,
	        .name        = "one",
	        .summary     = "One",
	        .kind        = howdy::native::CommandKind::kEntrypoint,
	        .user_target = howdy::native::UserTargetMode::kNone,
	    },
	    CommandDescriptor{
	        .id          = CommandId::kAdd,
	        .name        = "two",
	        .summary     = "Two",
	        .kind        = howdy::native::CommandKind::kEntrypoint,
	        .user_target = howdy::native::UserTargetMode::kNone,
	    },
	};
	ok &= expect(!render_command_reference(duplicate_command_ids).ok(),
	             "duplicate command IDs are rejected");
	const std::array duplicate_command_names{
	    CommandDescriptor{.id          = CommandId::kAdd,
	                      .name        = "same",
	                      .summary     = "One",
	                      .kind        = howdy::native::CommandKind::kEntrypoint,
	                      .user_target = howdy::native::UserTargetMode::kNone},
	    CommandDescriptor{.id          = CommandId::kClear,
	                      .name        = "same",
	                      .summary     = "Two",
	                      .kind        = howdy::native::CommandKind::kEntrypoint,
	                      .user_target = howdy::native::UserTargetMode::kNone},
	};
	ok &= expect(!render_command_reference(duplicate_command_names).ok(),
	             "duplicate command names are rejected");
	const std::array empty_command{
	    CommandDescriptor{.id          = CommandId::kAdd,
	                      .name        = "",
	                      .summary     = "Summary",
	                      .kind        = howdy::native::CommandKind::kEntrypoint,
	                      .user_target = howdy::native::UserTargetMode::kNone},
	};
	ok &= expect(!render_command_reference(empty_command).ok(), "empty command names are rejected");
	const std::array empty_summary{
	    CommandDescriptor{
	        .id          = CommandId::kAdd,
	        .name        = "name",
	        .summary     = "",
	        .kind        = howdy::native::CommandKind::kEntrypoint,
	        .user_target = howdy::native::UserTargetMode::kNone,
	    },
	};
	ok &= expect(!render_command_reference(empty_summary).ok(),
	             "empty command summaries are rejected");

	const std::array duplicate_options{
	    GlobalOptionDescriptor{.id            = howdy::native::GlobalOptionId::kUser,
	                           .short_name    = "-x",
	                           .long_name     = "",
	                           .argument_name = "",
	                           .summary       = "One"},
	    GlobalOptionDescriptor{.id            = howdy::native::GlobalOptionId::kPlain,
	                           .short_name    = "-x",
	                           .long_name     = "",
	                           .argument_name = "",
	                           .summary       = "Two"},
	};
	ok &= expect(!render_global_option_reference(duplicate_options).ok(),
	             "duplicate option spellings are rejected");
	const std::array duplicate_option_ids{
	    GlobalOptionDescriptor{.id            = howdy::native::GlobalOptionId::kUser,
	                           .short_name    = "-x",
	                           .long_name     = "",
	                           .argument_name = "",
	                           .summary       = "One"},
	    GlobalOptionDescriptor{.id            = howdy::native::GlobalOptionId::kUser,
	                           .short_name    = "-y",
	                           .long_name     = "",
	                           .argument_name = "",
	                           .summary       = "Two"},
	};
	ok &= expect(!render_global_option_reference(duplicate_option_ids).ok(),
	             "duplicate option IDs are rejected");
	const std::array duplicate_long_options{
	    GlobalOptionDescriptor{.id            = howdy::native::GlobalOptionId::kUser,
	                           .short_name    = "-x",
	                           .long_name     = "--same",
	                           .argument_name = "",
	                           .summary       = "One"},
	    GlobalOptionDescriptor{.id            = howdy::native::GlobalOptionId::kPlain,
	                           .short_name    = "-y",
	                           .long_name     = "--same",
	                           .argument_name = "",
	                           .summary       = "Two"},
	};
	ok &= expect(!render_global_option_reference(duplicate_long_options).ok(),
	             "duplicate long option spellings are rejected");
	const std::array short_long_collision{
	    GlobalOptionDescriptor{.id            = howdy::native::GlobalOptionId::kUser,
	                           .short_name    = "-x",
	                           .long_name     = "--one",
	                           .argument_name = "",
	                           .summary       = "One"},
	    GlobalOptionDescriptor{.id            = howdy::native::GlobalOptionId::kPlain,
	                           .short_name    = "-y",
	                           .long_name     = "-x",
	                           .argument_name = "",
	                           .summary       = "Two"},
	};
	ok &= expect(!render_global_option_reference(short_long_collision).ok(),
	             "short and long option spellings cannot collide");
	const std::array absent_aliases{
	    GlobalOptionDescriptor{.id            = howdy::native::GlobalOptionId::kUser,
	                           .short_name    = "-x",
	                           .long_name     = "",
	                           .argument_name = "USER",
	                           .summary       = "Short-only option"},
	    GlobalOptionDescriptor{.id            = howdy::native::GlobalOptionId::kPlain,
	                           .short_name    = "",
	                           .long_name     = "--plain",
	                           .argument_name = "",
	                           .summary       = "Long-only option"},
	};
	ok &= expect(render_global_option_reference(absent_aliases).ok(),
	             "empty option aliases do not collide");
	const std::array empty_option{
	    GlobalOptionDescriptor{.id            = howdy::native::GlobalOptionId::kUser,
	                           .short_name    = "",
	                           .long_name     = "",
	                           .argument_name = "",
	                           .summary       = "Summary"},
	};
	ok &= expect(!render_global_option_reference(empty_option).ok(),
	             "empty option spellings are rejected");
	const std::array empty_option_summary{
	    GlobalOptionDescriptor{.id            = howdy::native::GlobalOptionId::kUser,
	                           .short_name    = "-x",
	                           .long_name     = "",
	                           .argument_name = "",
	                           .summary       = ""},
	};
	ok &= expect(!render_global_option_reference(empty_option_summary).ok(),
	             "empty option summaries are rejected");

	const std::array duplicate_workarounds{
	    WorkaroundDescriptor{.value = "same", .workaround = Workaround::kInput, .summary = "One"},
	    WorkaroundDescriptor{.value = "same", .workaround = Workaround::kNative, .summary = "Two"},
	};
	ok &= expect(!render_workaround_reference(duplicate_workarounds, Workaround::kOff).ok(),
	             "duplicate workaround values are rejected");
	const std::array empty_workaround_summary{
	    WorkaroundDescriptor{.value = "value", .workaround = Workaround::kInput, .summary = ""},
	};
	ok &= expect(!render_workaround_reference(empty_workaround_summary, Workaround::kOff).ok(),
	             "empty workaround summaries are rejected");
	const std::array mapped_off{
	    WorkaroundDescriptor{.value = "off", .workaround = Workaround::kOff, .summary = "Off"},
	};
	ok &= expect(!render_workaround_reference(mapped_off, Workaround::kOff).ok(),
	             "off cannot become a mapped workaround value");

	return ok ? 0 : 1;
}
