#include "app/command_catalog.hpp"
#include "test_support.hpp"

#include <array>
#include <string_view>
#include <tuple>

namespace {

	using howdy::native::command_catalog;
	using howdy::native::CommandCompletionKind;
	using howdy::native::CommandDescriptor;
	using howdy::native::CommandId;
	using howdy::native::CommandKind;
	using howdy::native::find_command;
	using howdy::native::find_global_option;
	using howdy::native::global_option_catalog;
	using howdy::native::GlobalOptionCompletionKind;
	using howdy::native::GlobalOptionDescriptor;
	using howdy::native::GlobalOptionId;
	using howdy::native::UserTargetMode;
	using howdy::native::validate_command_catalog;
	using howdy::native::validate_global_option_catalog;
	using howdy::test::expect;

	auto test_global_options() -> bool {
		bool                                        ok = true;
		const std::array<GlobalOptionDescriptor, 0> empty_options{};
		ok &= expect(validate_global_option_catalog(empty_options).value_or("").contains("empty"),
		             "empty global option catalog is rejected");
		constexpr std::array expected_options{
		    std::tuple{GlobalOptionId::kUser, std::string_view{"-U"}, std::string_view{"--user"},
		               std::string_view{"USER"}, GlobalOptionCompletionKind::kUser, true},
		    std::tuple{GlobalOptionId::kPlain, std::string_view{""}, std::string_view{"--plain"},
		               std::string_view{""}, GlobalOptionCompletionKind::kNone, true},
		    std::tuple{GlobalOptionId::kYes, std::string_view{"-y"}, std::string_view{""},
		               std::string_view{""}, GlobalOptionCompletionKind::kNone, true},
		    std::tuple{GlobalOptionId::kHelp, std::string_view{"-h"}, std::string_view{"--help"},
		               std::string_view{""}, GlobalOptionCompletionKind::kNone, false},
		};
		const auto options = global_option_catalog();
		ok &= expect(options.size() == expected_options.size(),
		             "global option catalog size is stable");
		ok &= expect(options.size() == static_cast<std::size_t>(GlobalOptionId::kCount),
		             "every global option ID has a catalog slot");
		ok &= expect(!validate_global_option_catalog(options, true).has_value(),
		             "global option catalog invariants hold");
		ok &= expect(find_global_option("") == nullptr, "empty option spelling is not searchable");
		for (std::size_t index = 0; index < options.size(); ++index) {
			const auto &option                 = options[index];
			const auto &[id, short_name, long_name, argument_name, completion,
			             parses_after_command] = expected_options[index];
			ok &=
			    expect(option.id == id && option.short_name == short_name &&
			               option.long_name == long_name && option.argument_name == argument_name &&
			               option.completion == completion &&
			               option.parses_after_command == parses_after_command,
			           "global option syntax and completion metadata are stable");
			ok &= expect(find_global_option(option.id) == &option,
			             "global option ID resolves to its catalog entry");
			if (!option.short_name.empty()) {
				ok &= expect(find_global_option(option.short_name) == &option,
				             "short option lookup uses catalog entry");
			}
			if (!option.long_name.empty()) {
				ok &= expect(find_global_option(option.long_name) == &option,
				             "long option lookup uses catalog entry");
			}
		}
		const auto duplicate_ids = std::array{
		    GlobalOptionDescriptor{.id            = GlobalOptionId::kUser,
		                           .short_name    = "-a",
		                           .long_name     = "",
		                           .argument_name = "USER",
		                           .summary       = "One"},
		    GlobalOptionDescriptor{.id            = GlobalOptionId::kUser,
		                           .short_name    = "-b",
		                           .long_name     = "",
		                           .argument_name = "",
		                           .summary       = "Two"},
		};
		ok &=
		    expect(validate_global_option_catalog(duplicate_ids).value_or("").contains("duplicate"),
		           "duplicate global option IDs are rejected");
		const auto duplicate_spellings = std::array{
		    GlobalOptionDescriptor{.id            = GlobalOptionId::kUser,
		                           .short_name    = "-a",
		                           .long_name     = "--one",
		                           .argument_name = "USER",
		                           .summary       = "One"},
		    GlobalOptionDescriptor{.id            = GlobalOptionId::kPlain,
		                           .short_name    = "-b",
		                           .long_name     = "-a",
		                           .argument_name = "",
		                           .summary       = "Two"},
		};
		ok &=
		    expect(validate_global_option_catalog(duplicate_spellings).value_or("").contains("-a"),
		           "duplicate global option spellings are rejected");
		const auto no_spelling = std::array{
		    GlobalOptionDescriptor{.id            = GlobalOptionId::kUser,
		                           .short_name    = "",
		                           .long_name     = "",
		                           .argument_name = "",
		                           .summary       = "Option"},
		};
		ok &= expect(validate_global_option_catalog(no_spelling).value_or("").contains("spelling"),
		             "global option without spelling is rejected");
		const auto empty_option_summary = std::array{
		    GlobalOptionDescriptor{.id            = GlobalOptionId::kUser,
		                           .short_name    = "-a",
		                           .long_name     = "",
		                           .argument_name = "USER",
		                           .summary       = ""},
		};
		ok &= expect(
		    validate_global_option_catalog(empty_option_summary).value_or("").contains("summary"),
		    "empty global option summary is rejected");
		const auto invalid_option_completion = std::array{
		    GlobalOptionDescriptor{.id            = GlobalOptionId::kUser,
		                           .short_name    = "-a",
		                           .long_name     = "",
		                           .argument_name = "USER",
		                           .summary       = "Option",
		                           .completion    = static_cast<GlobalOptionCompletionKind>(255)},
		};
		ok &= expect(validate_global_option_catalog(invalid_option_completion)
		                 .value_or("")
		                 .contains("completion"),
		             "unknown global option completion kind is rejected");
		const auto invalid_option_id = std::array{
		    GlobalOptionDescriptor{.id            = static_cast<GlobalOptionId>(255),
		                           .short_name    = "-a",
		                           .long_name     = "",
		                           .argument_name = "",
		                           .summary       = "Option"},
		};
		ok &= expect(
		    validate_global_option_catalog(invalid_option_id).value_or("").contains("invalid id"),
		    "invalid global option ID is rejected");
		const auto missing_option_id = std::array{
		    GlobalOptionDescriptor{.id            = GlobalOptionId::kUser,
		                           .short_name    = "-a",
		                           .long_name     = "",
		                           .argument_name = "USER",
		                           .summary       = "Option"},
		};
		ok &= expect(validate_global_option_catalog(missing_option_id, true)
		                 .value_or("")
		                 .contains("missing"),
		             "missing production global option ID is rejected");
		const auto repeated_alias = std::array{
		    GlobalOptionDescriptor{.id            = GlobalOptionId::kUser,
		                           .short_name    = "-a",
		                           .long_name     = "-a",
		                           .argument_name = "USER",
		                           .summary       = "Option"},
		};
		ok &=
		    expect(validate_global_option_catalog(repeated_alias).value_or("").contains("repeats"),
		           "global option aliases cannot repeat");
		const auto missing_option_argument = std::array{
		    GlobalOptionDescriptor{.id            = GlobalOptionId::kUser,
		                           .short_name    = "-a",
		                           .long_name     = "",
		                           .argument_name = "",
		                           .summary       = "Option",
		                           .completion    = GlobalOptionCompletionKind::kUser},
		};
		ok &= expect(validate_global_option_catalog(missing_option_argument)
		                 .value_or("")
		                 .contains("argument"),
		             "argument completion requires argument metadata");
		const auto unsupported_option_argument = std::array{
		    GlobalOptionDescriptor{.id            = GlobalOptionId::kPlain,
		                           .short_name    = "",
		                           .long_name     = "--plain",
		                           .argument_name = "VALUE",
		                           .summary       = "Option"},
		};
		ok &= expect(validate_global_option_catalog(unsupported_option_argument)
		                 .value_or("")
		                 .contains("unsupported"),
		             "unsupported option argument metadata is rejected");
		const auto misplaced_user_completion = std::array{
		    GlobalOptionDescriptor{.id            = GlobalOptionId::kPlain,
		                           .short_name    = "",
		                           .long_name     = "--plain",
		                           .argument_name = "VALUE",
		                           .summary       = "Option",
		                           .completion    = GlobalOptionCompletionKind::kUser},
		};
		ok &= expect(validate_global_option_catalog(misplaced_user_completion)
		                 .value_or("")
		                 .contains("user completion"),
		             "user completion requires user option metadata");
		return ok;
	}

	auto test_command_catalog_validation() -> bool {
		bool                                   ok = true;
		const std::array<CommandDescriptor, 0> empty_commands{};
		ok &= expect(validate_command_catalog(empty_commands).value_or("").contains("empty"),
		             "empty command catalog is rejected");
		ok &= expect(!validate_command_catalog(command_catalog(), true).has_value(),
		             "production command catalog is valid and complete");
		const auto duplicate_ids = std::array{
		    CommandDescriptor{.id          = CommandId::kAdd,
		                      .name        = "one",
		                      .summary     = "One",
		                      .kind        = CommandKind::kEntrypoint,
		                      .user_target = UserTargetMode::kNone},
		    CommandDescriptor{.id          = CommandId::kAdd,
		                      .name        = "two",
		                      .summary     = "Two",
		                      .kind        = CommandKind::kEntrypoint,
		                      .user_target = UserTargetMode::kNone},
		};
		ok &= expect(validate_command_catalog(duplicate_ids).value_or("").contains("duplicate"),
		             "duplicate command IDs are rejected");
		const auto duplicate_names = std::array{
		    CommandDescriptor{.id          = CommandId::kAdd,
		                      .name        = "same",
		                      .summary     = "One",
		                      .kind        = CommandKind::kEntrypoint,
		                      .user_target = UserTargetMode::kNone},
		    CommandDescriptor{.id          = CommandId::kClear,
		                      .name        = "same",
		                      .summary     = "Two",
		                      .kind        = CommandKind::kEntrypoint,
		                      .user_target = UserTargetMode::kNone},
		};
		ok &= expect(validate_command_catalog(duplicate_names).value_or("").contains("name"),
		             "duplicate command names are rejected");
		const auto invalid_id = std::array{
		    CommandDescriptor{.id          = static_cast<CommandId>(255),
		                      .name        = "invalid",
		                      .summary     = "Invalid",
		                      .kind        = CommandKind::kEntrypoint,
		                      .user_target = UserTargetMode::kNone},
		};
		ok &= expect(validate_command_catalog(invalid_id).value_or("").contains("invalid id"),
		             "invalid command ID is rejected");
		const auto empty_name = std::array{
		    CommandDescriptor{.id          = CommandId::kAdd,
		                      .name        = "",
		                      .summary     = "Summary",
		                      .kind        = CommandKind::kEntrypoint,
		                      .user_target = UserTargetMode::kNone},
		};
		ok &= expect(validate_command_catalog(empty_name).value_or("").contains("name"),
		             "empty command name is rejected");
		const auto empty_summary = std::array{
		    CommandDescriptor{.id          = CommandId::kAdd,
		                      .name        = "name",
		                      .summary     = "",
		                      .kind        = CommandKind::kEntrypoint,
		                      .user_target = UserTargetMode::kNone},
		};
		ok &= expect(validate_command_catalog(empty_summary).value_or("").contains("summary"),
		             "empty command summary is rejected");
		const auto invalid_metadata = std::array{
		    CommandDescriptor{.id          = CommandId::kAdd,
		                      .name        = "invalid",
		                      .summary     = "Invalid",
		                      .kind        = static_cast<CommandKind>(255),
		                      .user_target = UserTargetMode::kNone},
		};
		ok &= expect(validate_command_catalog(invalid_metadata).value_or("").contains("kind"),
		             "unknown command kind is rejected");
		const auto invalid_user_target = std::array{
		    CommandDescriptor{.id          = CommandId::kAdd,
		                      .name        = "invalid",
		                      .summary     = "Invalid",
		                      .kind        = CommandKind::kEntrypoint,
		                      .user_target = static_cast<UserTargetMode>(255)},
		};
		ok &= expect(
		    validate_command_catalog(invalid_user_target).value_or("").contains("user-target"),
		    "unknown command user-target mode is rejected");
		const auto invalid_completion = std::array{
		    CommandDescriptor{.id          = CommandId::kAdd,
		                      .name        = "invalid",
		                      .summary     = "Invalid",
		                      .kind        = CommandKind::kEntrypoint,
		                      .user_target = UserTargetMode::kNone,
		                      .completion  = static_cast<CommandCompletionKind>(255)},
		};
		ok &=
		    expect(validate_command_catalog(invalid_completion).value_or("").contains("completion"),
		           "unknown command completion kind is rejected");
		const auto reusable_boolean_completion = std::array{
		    CommandDescriptor{.id          = CommandId::kAdd,
		                      .name        = "add",
		                      .summary     = "Add",
		                      .kind        = CommandKind::kEntrypoint,
		                      .user_target = UserTargetMode::kNone,
		                      .completion  = CommandCompletionKind::kBoolean},
		};
		ok &= expect(!validate_command_catalog(reusable_boolean_completion).has_value(),
		             "boolean completion kind is reusable metadata");
		const auto reusable_config_completion = std::array{
		    CommandDescriptor{.id          = CommandId::kAdd,
		                      .name        = "add",
		                      .summary     = "Add",
		                      .kind        = CommandKind::kEntrypoint,
		                      .user_target = UserTargetMode::kNone,
		                      .completion  = CommandCompletionKind::kConfigSet},
		};
		ok &= expect(!validate_command_catalog(reusable_config_completion).has_value(),
		             "config completion kind is reusable metadata");
		const auto reusable_version_kind = std::array{
		    CommandDescriptor{.id          = CommandId::kAdd,
		                      .name        = "example",
		                      .summary     = "Example",
		                      .kind        = CommandKind::kVersion,
		                      .user_target = UserTargetMode::kNone},
		};
		ok &= expect(!validate_command_catalog(reusable_version_kind).has_value(),
		             "version command kind is reusable metadata");
		const auto invalid_version_target = std::array{
		    CommandDescriptor{.id          = CommandId::kAdd,
		                      .name        = "example",
		                      .summary     = "Example",
		                      .kind        = CommandKind::kVersion,
		                      .user_target = UserTargetMode::kModelUser},
		};
		ok &=
		    expect(validate_command_catalog(invalid_version_target).value_or("").contains("target"),
		           "version command user-target mismatch is rejected");
		const auto invalid_version_completion = std::array{
		    CommandDescriptor{.id          = CommandId::kAdd,
		                      .name        = "example",
		                      .summary     = "Example",
		                      .kind        = CommandKind::kVersion,
		                      .user_target = UserTargetMode::kNone,
		                      .completion  = CommandCompletionKind::kBoolean},
		};
		ok &= expect(validate_command_catalog(invalid_version_completion)
		                 .value_or("")
		                 .contains("completion values"),
		             "version command completion mismatch is rejected");
		const auto missing_id = std::array{
		    CommandDescriptor{.id          = CommandId::kAdd,
		                      .name        = "only",
		                      .summary     = "Only",
		                      .kind        = CommandKind::kEntrypoint,
		                      .user_target = UserTargetMode::kNone},
		};
		ok &= expect(validate_command_catalog(missing_id, true).value_or("").contains("missing"),
		             "missing production command ID is rejected");
		return ok;
	}

}  // namespace

auto main() -> int {
	bool ok = true;

	constexpr std::array expected_command_ids{
	    CommandId::kAdd,
	    CommandId::kClear,
	    CommandId::kConfig,
	    CommandId::kDisable,
	    CommandId::kDownloadModels,
	    CommandId::kList,
	    CommandId::kRemove,
	    CommandId::kSet,
	    CommandId::kSnapshot,
	    CommandId::kTest,
	    CommandId::kVersion,
	};
	constexpr std::array expected_command_kinds{
	    CommandKind::kEntrypoint, CommandKind::kEntrypoint, CommandKind::kEntrypoint,
	    CommandKind::kEntrypoint, CommandKind::kEntrypoint, CommandKind::kEntrypoint,
	    CommandKind::kEntrypoint, CommandKind::kEntrypoint, CommandKind::kEntrypoint,
	    CommandKind::kEntrypoint, CommandKind::kVersion,
	};
	constexpr std::array expected_completions{
	    CommandCompletionKind::kNone, CommandCompletionKind::kNone,
	    CommandCompletionKind::kNone, CommandCompletionKind::kBoolean,
	    CommandCompletionKind::kNone, CommandCompletionKind::kNone,
	    CommandCompletionKind::kNone, CommandCompletionKind::kConfigSet,
	    CommandCompletionKind::kNone, CommandCompletionKind::kNone,
	    CommandCompletionKind::kNone,
	};
	constexpr std::array expected_user_targets{
	    UserTargetMode::kModelUser, UserTargetMode::kModelUser, UserTargetMode::kNone,
	    UserTargetMode::kNone,      UserTargetMode::kNone,      UserTargetMode::kModelUser,
	    UserTargetMode::kModelUser, UserTargetMode::kNone,      UserTargetMode::kNone,
	    UserTargetMode::kModelUser, UserTargetMode::kNone,
	};
	const auto commands = command_catalog();
	ok &= expect(commands.size() == expected_command_ids.size(), "command catalog size is stable");
	ok &= expect(commands.size() == static_cast<std::size_t>(CommandId::kCount),
	             "every command ID has a catalog slot");
	for (std::size_t index = 0; index < commands.size(); ++index) {
		const auto &command = commands[index];
		ok &=
		    expect(index < expected_command_ids.size() && command.id == expected_command_ids[index],
		           "command catalog order is stable");
		ok &= expect(index < expected_command_kinds.size() &&
		                 command.kind == expected_command_kinds[index],
		             "command kind is stable");
		ok &= expect(index < expected_completions.size() &&
		                 command.completion == expected_completions[index],
		             "command completion metadata is stable");
		ok &= expect(index < expected_user_targets.size() &&
		                 command.user_target == expected_user_targets[index],
		             "command user-target behavior is stable");
		ok &= expect(find_command(command.name) == &command, "command lookup uses catalog entry");
	}
	ok &= expect(find_command("unknown") == nullptr, "unknown command has no completion metadata");

	ok &= test_command_catalog_validation();
	ok &= test_global_options();

	return ok ? 0 : 1;
}
