#include "app/command_catalog.hpp"
#include "config/config_schema.hpp"
#include "docs/man_reference.hpp"
#include "module/pam_option_catalog.hpp"
#include "test_support.hpp"
#include "vision/capture_device_path.hpp"

#include <array>
#include <string>
#include <string_view>

namespace {

	using howdy::docs::render_command_reference;
	using howdy::docs::render_config_option_reference;
	using howdy::docs::render_global_option_reference;
	using howdy::docs::render_workaround_reference;
	using howdy::native::CommandDescriptor;
	using howdy::native::CommandId;
	using howdy::native::GlobalOptionDescriptor;
	using howdy::native::config_schema::Option;
	using howdy::native::config_schema::OptionId;
	using howdy::native::config_schema::RuntimeDefault;
	using howdy::native::config_schema::SpecialRule;
	using howdy::native::config_schema::ValueType;
	using howdy::pam::Workaround;
	using howdy::pam::WorkaroundDescriptor;
	using howdy::test::expect;

	auto has_exactly_one_final_newline(std::string_view value) -> bool {
		return !value.empty() && value.back() == '\n' &&
		       (value.size() == 1 || value[value.size() - 2] != '\n');
	}

	auto escape_for_roff(std::string_view value) -> std::string {
		std::string escaped;
		escaped.reserve(value.size());
		for (const char character : value) {
			switch (character) {
				case '\\':
					escaped += "\\\\";
					break;
				case '-':
					escaped += "\\-";
					break;
				default:
					escaped += character;
					break;
			}
		}
		return escaped;
	}

	auto synthetic_config_option(OptionId id, std::string_view section, std::string_view key,
	                             ValueType type, RuntimeDefault fallback,
	                             std::string_view description) -> Option {
		return Option{.id           = id,
		              .section      = section,
		              .key          = key,
		              .type         = type,
		              .fallback     = fallback,
		              .range        = {},
		              .choices      = {},
		              .special_rule = SpecialRule::none,
		              .invalid_rule = "synthetic rule",
		              .description  = description};
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

	const auto production_schema_options = howdy::native::config_schema::runtime_config_options();
	const auto config_result = render_config_option_reference(production_schema_options);
	ok &= expect(config_result.ok(), "production config options render");
	ok &= expect(has_exactly_one_final_newline(config_result.output),
	             "config reference has one final newline");
	ok &= expect(config_result.output.contains(".SS [core]\n"),
	             "config reference renders core section");
	ok &= expect(config_result.output.contains(".SS [video]\n"),
	             "config reference renders video section");
	ok &= expect(config_result.output.contains(".SS [face]\n"),
	             "config reference renders face section");
	ok &= expect(config_result.output.contains(".SS [debug]\n"),
	             "config reference renders debug section");

	const auto core_pos  = config_result.output.find(".SS [core]\n");
	const auto video_pos = config_result.output.find(".SS [video]\n");
	const auto face_pos  = config_result.output.find(".SS [face]\n");
	const auto debug_pos = config_result.output.find(".SS [debug]\n");
	ok &= expect(core_pos != std::string::npos && video_pos != std::string::npos &&
	                 face_pos != std::string::npos && debug_pos != std::string::npos &&
	                 core_pos < video_pos && video_pos < face_pos && face_pos < debug_pos,
	             "sections appear in schema order");

	for (const auto &opt : production_schema_options) {
		const auto key_tag = "\\&\\fB" + std::string(opt.key) + "\\fR";
		ok &= expect(config_result.output.contains(key_tag),
		             "config option key tag appears: " + std::string(opt.key));
		ok &= expect(config_result.output.contains(escape_for_roff(opt.description)),
		             "config option description appears: " + std::string(opt.key));
	}

	ok &= expect(!config_result.output.contains("Default:"),
	             "config reference does not contain Default: field");

	ok &=
	    expect(config_result.output.contains("\n.br\n\\&Range: 1..300.\n"),
	           "video timeout range renders on separate metadata line without default annotation");
	ok &= expect(
	    config_result.output.contains("\n.br\n\\&Accepted: \\fBnone\\fR, /dev/video*, "
	                                  "/dev/v4l/by\\-path/*, /dev/v4l/by\\-id/*.\n"),
	    "video device_path renders accepted patterns with bold fallback token on separate line");
	ok &= expect(config_result.output.contains("\n.br\n\\&Range: \\-1 or 16..8192.\n"),
	             "video frame_width sentinel range renders on separate line");
	ok &= expect(config_result.output.contains("\n.br\n\\&Choices: \\fBcosine\\fR, l2, l2norm.\n"),
	             "face sface_metric choices renders with bold fallback token on separate line");
	ok &= expect(config_result.output.contains(
	                 "\n.br\n\\&Range: 0..1 for cosine, 0..4 for l2 and l2norm.\n"),
	             "face sface_threshold metric-dependent range renders on separate line without "
	             "default annotation");
	ok &= expect(config_result.output.contains(
	                 "\n.br\n\\&Values: true, \\fBfalse\\fR, 1, 0, yes, no, on, off.\n"),
	             "boolean option with false default bolds false token");
	ok &= expect(config_result.output.contains(
	                 "\n.br\n\\&Values: \\fBtrue\\fR, false, 1, 0, yes, no, on, off.\n"),
	             "boolean option with true default bolds true token");

	for (const auto &spelling : howdy::native::config_schema::kAcceptedBooleanSpellings) {
		ok &= expect(config_result.output.contains(spelling),
		             "boolean option documentation includes accepted spelling: " +
		                 std::string(spelling));
	}
	for (const auto &pattern : howdy::native::kAcceptedCaptureDevicePatterns) {
		ok &=
		    expect(config_result.output.contains(escape_for_roff(pattern)),
		           "device_path documentation includes accepted pattern: " + std::string(pattern));
	}

	const auto repeat_config_result = render_config_option_reference(production_schema_options);
	ok &= expect(config_result.output == repeat_config_result.output,
	             "config option rendering is deterministic");

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

	const std::array escaped_config_options = {
	    synthetic_config_option(OptionId::core_detection_notice, "sec-a", "key-a",
	                            ValueType::boolean,
	                            howdy::native::config_schema::bool_default(true), "Desc a-b\\c."),
	};
	const auto escaped_config_res = render_config_option_reference(escaped_config_options);
	ok &= expect(escaped_config_res.ok(), "escaped config options render");
	ok &= expect(escaped_config_res.output.contains(".SS [sec\\-a]\n"),
	             "escaped config section escapes hyphens");
	ok &= expect(escaped_config_res.output.contains(R"(\&\fBkey\-a\fR)"),
	             "escaped config key escapes hyphens");
	ok &= expect(escaped_config_res.output.contains(R"(Desc a\-b\\c.)"),
	             "escaped config description escapes hyphens and backslashes");

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

	const std::array control_config_section = {
	    synthetic_config_option(OptionId::core_detection_notice, "core\n", "key",
	                            ValueType::boolean,
	                            howdy::native::config_schema::bool_default(true), "Valid desc."),
	};
	ok &= expect(!render_config_option_reference(control_config_section).ok(),
	             "control characters in config section are rejected");

	const std::array control_config_key = {
	    synthetic_config_option(OptionId::core_detection_notice, "core", "key\n",
	                            ValueType::boolean,
	                            howdy::native::config_schema::bool_default(true), "Valid desc."),
	};
	ok &= expect(!render_config_option_reference(control_config_key).ok(),
	             "control characters in config key are rejected");

	const std::array control_config_desc = {
	    synthetic_config_option(OptionId::core_detection_notice, "core", "key", ValueType::boolean,
	                            howdy::native::config_schema::bool_default(true), "Desc\n"),
	};
	ok &= expect(!render_config_option_reference(control_config_desc).ok(),
	             "control characters in config description are rejected");

	const std::array non_contiguous_sections = {
	    synthetic_config_option(OptionId::core_detection_notice, "sec1", "k1", ValueType::boolean,
	                            howdy::native::config_schema::bool_default(true), "Desc 1."),
	    synthetic_config_option(OptionId::core_no_confirmation, "sec2", "k2", ValueType::boolean,
	                            howdy::native::config_schema::bool_default(true), "Desc 2."),
	    synthetic_config_option(OptionId::core_abort_if_ssh, "sec1", "k3", ValueType::boolean,
	                            howdy::native::config_schema::bool_default(true), "Desc 3."),
	};
	ok &= expect(!render_config_option_reference(non_contiguous_sections).ok(),
	             "non-contiguous config section reuse is rejected");

	const std::array<Option, 0> empty_config_schema = {};
	ok &= expect(!render_config_option_reference(empty_config_schema).ok(),
	             "empty config schema is rejected");

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
