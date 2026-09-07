#include "app/command_catalog.hpp"
#include "completion/internal.hpp"
#include "config/config_schema.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace howdy::native::howdy_completion_internal {
	namespace {

		constexpr std::array<std::string_view, 2> kBooleanCompletionValues{"false", "true"};

		auto PrintCompletionCommands(std::span<const std::string> required_options = {}) -> bool {
			std::vector<GlobalOptionId> required_option_ids;
			required_option_ids.reserve(required_options.size());
			for (const auto &spelling : required_options) {
				const auto *option = FindGlobalOption(spelling);
				if (option == nullptr || !option->parses_after_command) {
					return false;
				}
				required_option_ids.push_back(option->id);
			}

			for (const auto &descriptor : CommandCatalog()) {
				if (std::ranges::all_of(required_option_ids, [&](const auto id) -> auto {
					    return CommandAcceptsGlobalOption(descriptor, id);
				    })) {
					std::cout << descriptor.name << '\n';
				}
			}
			return true;
		}

		auto CompletionKindName(GlobalOptionCompletionKind kind) -> std::string_view {
			switch (kind) {
				case GlobalOptionCompletionKind::kNone:
					return "none";
				case GlobalOptionCompletionKind::kUser:
					return "user";
			}
			return "none";
		}

		void PrintCompletionGlobalOptions() {
			for (const auto &option : GlobalOptionCatalog()) {
				for (const auto spelling : {option.short_name, option.long_name}) {
					if (!spelling.empty()) {
						std::cout << spelling << '\t' << (!option.argument_name.empty() ? '1' : '0')
						          << '\t' << (option.parses_after_command ? '1' : '0') << '\t'
						          << CompletionKindName(option.completion) << '\n';
					}
				}
			}
		}

		void PrintCompletionCommandMaxPositionals(std::string_view command_name) {
			const auto *descriptor = FindCommand(command_name);
			if (descriptor == nullptr) {
				return;
			}
			std::cout << descriptor->max_positionals << '\n';
		}

		void PrintCompletionCommandOptions(std::string_view command_name) {
			const auto *descriptor = FindCommand(command_name);
			if (descriptor == nullptr) {
				return;
			}
			for (const auto &option : GlobalOptionCatalog()) {
				if (option.id != GlobalOptionId::kHelp &&
				    (!option.parses_after_command ||
				     !CommandAcceptsGlobalOption(*descriptor, option.id))) {
					continue;
				}
				for (const auto spelling : {option.short_name, option.long_name}) {
					if (!spelling.empty()) {
						std::cout << spelling << '\t' << (!option.argument_name.empty() ? '1' : '0')
						          << "\tnone\n";
					}
				}
			}
			for (const auto &option : descriptor->options) {
				for (const auto spelling : {option.short_name, option.long_name}) {
					if (!spelling.empty()) {
						std::cout << spelling << '\t' << (!option.argument_name.empty() ? '1' : '0')
						          << "\tnone\n";
					}
				}
			}
		}

		auto ParseCompletionPosition(std::string_view text, std::size_t &position) -> bool {
			if (text.empty()) {
				return false;
			}
			const auto result = std::from_chars(text.data(), text.data() + text.size(), position);
			return result.ec == std::errc{} && result.ptr == text.data() + text.size();
		}

		void PrintConfigOptionValues(const config_schema::Option &option) {
			if (!option.choices.empty()) {
				for (const auto choice : option.choices) {
					std::cout << choice << '\n';
				}
				return;
			}
			switch (option.type) {
				case config_schema::ValueType::kBoolean:
					for (const auto value : kBooleanCompletionValues) {
						std::cout << value << '\n';
					}
					return;
				case config_schema::ValueType::kInteger:
				case config_schema::ValueType::kFloatingPoint:
				case config_schema::ValueType::kString:
					return;
			}
		}

		auto PrintCompletionCommandValues(std::string_view command_name, std::size_t position,
		                                  std::span<const std::string> previous_positionals)
		    -> bool {
			const auto *descriptor = FindCommand(command_name);
			if (descriptor == nullptr || previous_positionals.size() != position) {
				return false;
			}
			if (descriptor->completion == CommandCompletionKind::kBoolean) {
				if (position != 0) {
					return false;
				}
				for (const auto value : kBooleanCompletionValues) {
					std::cout << value << '\n';
				}
				return true;
			}
			if (descriptor->completion != CommandCompletionKind::kConfigSet) {
				return position == 0;
			}
			if (position == 0) {
				for (const auto &option : config_schema::RuntimeConfigOptions()) {
					std::cout << option.key << '\n';
				}
				return true;
			}
			if (position != 1) {
				return false;
			}
			const auto &key = previous_positionals.front();
			for (const auto &option : config_schema::RuntimeConfigOptions()) {
				if (option.key == key) {
					PrintConfigOptionValues(option);
					break;
				}
			}
			return true;
		}

	}  // namespace

	auto HandleCompletionQuery(const std::vector<std::string> &arguments, bool global_option_seen)
	    -> int {
		if (global_option_seen) {
			return 1;
		}
		if (!arguments.empty() && arguments.front() == "commands") {
			return PrintCompletionCommands(std::span<const std::string>(arguments).subspan(1)) ? 0
			                                                                                   : 1;
		}
		if (arguments.size() == 1 && arguments.front() == "global-options") {
			PrintCompletionGlobalOptions();
			return 0;
		}
		if (arguments.size() == 2 && arguments.front() == "command-options") {
			PrintCompletionCommandOptions(arguments[1]);
			return FindCommand(arguments[1]) == nullptr ? 1 : 0;
		}
		if (arguments.size() == 2 && arguments.front() == "command-max-positionals") {
			PrintCompletionCommandMaxPositionals(arguments[1]);
			return FindCommand(arguments[1]) == nullptr ? 1 : 0;
		}
		if (arguments.size() >= 3 && arguments.front() == "command-values") {
			std::size_t position = 0;
			if (ParseCompletionPosition(arguments[2], position) &&
			    position == arguments.size() - 3 &&
			    PrintCompletionCommandValues(arguments[1], position,
			                                 std::span<const std::string>(arguments).subspan(3))) {
				return 0;
			}
		}
		return 1;
	}

}  // namespace howdy::native::howdy_completion_internal
