#include "app/command_catalog.hpp"
#include "test_support.hpp"

#include <array>
#include <string_view>
#include <tuple>

namespace {

	using howdy::native::command_catalog;
	using howdy::native::CommandCompletionKind;
	using howdy::native::CommandId;
	using howdy::native::CommandKind;
	using howdy::native::find_command;
	using howdy::native::find_global_option;
	using howdy::native::global_option_catalog;
	using howdy::native::global_option_catalog_is_valid;
	using howdy::native::GlobalOptionCompletionKind;
	using howdy::native::GlobalOptionId;
	using howdy::native::UserTargetMode;
	using howdy::test::expect;

	auto test_global_options() -> bool {
		bool                 ok = true;
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
		ok &= expect(global_option_catalog_is_valid(), "global option catalog invariants hold");
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
			ok &= expect(!option.summary.empty(), "global option summary is non-empty");
			ok &= expect(!option.short_name.empty() || !option.long_name.empty(),
			             "global option has a non-empty spelling");
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
			for (std::size_t previous = 0; previous < index; ++previous) {
				if (!option.short_name.empty()) {
					ok &= expect((options[previous].short_name.empty() ||
					              option.short_name != options[previous].short_name) &&
					                 (options[previous].long_name.empty() ||
					                  option.short_name != options[previous].long_name),
					             "short option spellings are unique");
				}
				if (!option.long_name.empty()) {
					ok &= expect((options[previous].short_name.empty() ||
					              option.long_name != options[previous].short_name) &&
					                 (options[previous].long_name.empty() ||
					                  option.long_name != options[previous].long_name),
					             "long option spellings are unique");
				}
			}
		}
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
		ok &= expect(!command.name.empty(), "command name is non-empty");
		ok &= expect(!command.summary.empty(), "command summary is non-empty");
		ok &= expect(find_command(command.name) == &command, "command lookup uses catalog entry");
		for (std::size_t previous = 0; previous < index; ++previous) {
			ok &= expect(commands[previous].id != command.id, "command IDs are unique");
			ok &= expect(commands[previous].name != command.name, "command names are unique");
		}
	}
	ok &= expect(find_command("unknown") == nullptr, "unknown command has no completion metadata");

	ok &= test_global_options();

	return ok ? 0 : 1;
}
