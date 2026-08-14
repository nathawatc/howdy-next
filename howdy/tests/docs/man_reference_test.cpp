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
	ok &= expect(command_result.output.contains(R"(\&\fBadd [LABEL]\fR)"),
	             "command reference renders optional positional syntax");
	ok &= expect(command_result.output.contains("Add face model"),
	             "command reference renders command summary");
	ok &= expect(command_result.output.contains(R"(\&\fBdisable {0|1|true|false}\fR)"),
	             "command reference renders finite-value syntax");
	ok &= expect(command_result.output.contains(R"(\&\fBremove ID\fR)"),
	             "command reference renders required positional syntax");
	ok &= expect(command_result.output.contains(R"(\&\fBset KEY VALUE\fR)"),
	             "command reference renders multiple positional syntax");
	ok &= expect(command_result.output.contains(R"(\&\fBtest [\-\-device DEVICE]\fR)"),
	             "command reference renders command-local option syntax");
	ok &= expect(command_result.output.contains(R"(\&\fBclear\fR)"),
	             "command reference keeps no-argument command bare");
	const auto misleading_phrase = std::string{"target user"} + " required";
	ok &= expect(!command_result.output.contains(misleading_phrase),
	             "command reference does not imply explicit user input is required");
	ok &= expect(!command_result.output.contains(R"(\&\fBremove USER)"),
	             "command reference does not advertise injected model user");
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

	const std::array generic_commands{
	    CommandDescriptor{
	        .id                = CommandId::kAdd,
	        .name              = "sample",
	        .summary           = "Sample command",
	        .kind              = howdy::native::CommandKind::kEntrypoint,
	        .user_target       = howdy::native::UserTargetMode::kNone,
	        .completion        = howdy::native::CommandCompletionKind::kNone,
	        .argument_synopsis = "[VALUE]",
	    },
	};
	const auto generic_result = render_command_reference(generic_commands);
	ok &= expect(generic_result.ok(), "generic command descriptor renders");
	ok &= expect(generic_result.output.contains(R"(\&\fBsample [VALUE]\fR)"),
	             "renderer uses descriptor-provided argument synopsis");

	const std::array escaped_commands{
	    CommandDescriptor{
	        .id                = CommandId::kAdd,
	        .name              = "a-b\\c",
	        .summary           = "A-b\\c",
	        .kind              = howdy::native::CommandKind::kEntrypoint,
	        .user_target       = howdy::native::UserTargetMode::kNone,
	        .completion        = howdy::native::CommandCompletionKind::kNone,
	        .argument_synopsis = "[A-B\\C]",
	    },
	};
	const auto escaped_result = render_command_reference(escaped_commands);
	ok &= expect(escaped_result.ok(), "safe roff metadata renders");
	ok &= expect(escaped_result.output.contains(R"(a\-b\\c)"),
	             "roff escapes hyphens and backslashes");
	ok &= expect(escaped_result.output.contains(R"([A\-B\\C])"),
	             "roff escapes synopsis hyphens and backslashes");

	const std::array control_command{
	    CommandDescriptor{.id          = CommandId::kAdd,
	                      .name        = "add\n",
	                      .summary     = "Add face model",
	                      .kind        = howdy::native::CommandKind::kEntrypoint,
	                      .user_target = howdy::native::UserTargetMode::kNone},
	};
	ok &= expect(!render_command_reference(control_command).ok(),
	             "control characters in command text are rejected");
	const std::array control_synopsis{
	    CommandDescriptor{.id                = CommandId::kAdd,
	                      .name              = "sample",
	                      .summary           = "Sample command",
	                      .kind              = howdy::native::CommandKind::kEntrypoint,
	                      .user_target       = howdy::native::UserTargetMode::kNone,
	                      .completion        = howdy::native::CommandCompletionKind::kNone,
	                      .argument_synopsis = "VALUE\n"},
	};
	const auto control_synopsis_result = render_command_reference(control_synopsis);
	ok &= expect(!control_synopsis_result.ok() &&
	                 control_synopsis_result.error.contains("argument synopsis"),
	             "control characters in argument synopsis are rejected");
	const std::array control_option{
	    GlobalOptionDescriptor{.id            = howdy::native::GlobalOptionId::kUser,
	                           .short_name    = "-x",
	                           .long_name     = "",
	                           .argument_name = "USER",
	                           .summary       = "Summary\n"},
	};
	const auto control_option_result = render_global_option_reference(control_option);
	ok &= expect(!control_option_result.ok() &&
	                 control_option_result.error.contains("control character"),
	             "control characters in option text are rejected");

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
