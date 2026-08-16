#include "app/command_catalog.hpp"
#include "app/howdy_completion_internal.hpp"
#include "config/config_schema.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace howdy::native::howdy_completion_internal {
	namespace {

		constexpr std::array<std::string_view, 2> kBooleanCompletionValues{"false", "true"};

		auto print_completion_commands(std::span<const std::string> required_options = {}) -> bool {
			std::vector<GlobalOptionId> required_option_ids;
			required_option_ids.reserve(required_options.size());
			for (const auto &spelling : required_options) {
				const auto *option = find_global_option(spelling);
				if (option == nullptr || !option->parses_after_command) {
					return false;
				}
				required_option_ids.push_back(option->id);
			}

			for (const auto &descriptor : command_catalog()) {
				if (std::ranges::all_of(required_option_ids, [&](const auto id) -> auto {
					    return command_accepts_global_option(descriptor, id);
				    })) {
					std::cout << descriptor.name << '\n';
				}
			}
			return true;
		}

		auto completion_kind_name(GlobalOptionCompletionKind kind) -> std::string_view {
			switch (kind) {
				case GlobalOptionCompletionKind::kNone:
					return "none";
				case GlobalOptionCompletionKind::kUser:
					return "user";
			}
			return "none";
		}

		void print_completion_global_options() {
			for (const auto &option : global_option_catalog()) {
				for (const auto spelling : {option.short_name, option.long_name}) {
					if (!spelling.empty()) {
						std::cout << spelling << '\t' << (!option.argument_name.empty() ? '1' : '0')
						          << '\t' << (option.parses_after_command ? '1' : '0') << '\t'
						          << completion_kind_name(option.completion) << '\n';
					}
				}
			}
		}

		void print_completion_command_max_positionals(std::string_view command_name) {
			const auto *descriptor = find_command(command_name);
			if (descriptor == nullptr) {
				return;
			}
			std::cout << descriptor->max_positionals << '\n';
		}

		void print_completion_command_options(std::string_view command_name) {
			const auto *descriptor = find_command(command_name);
			if (descriptor == nullptr) {
				return;
			}
			for (const auto &option : global_option_catalog()) {
				if (option.id != GlobalOptionId::kHelp &&
				    (!option.parses_after_command ||
				     !command_accepts_global_option(*descriptor, option.id))) {
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

		auto parse_completion_position(std::string_view text, std::size_t &position) -> bool {
			if (text.empty()) {
				return false;
			}
			const auto result = std::from_chars(text.data(), text.data() + text.size(), position);
			return result.ec == std::errc{} && result.ptr == text.data() + text.size();
		}

		void print_config_option_values(const config_schema::Option &option) {
			if (!option.choices.empty()) {
				for (const auto choice : option.choices) {
					std::cout << choice << '\n';
				}
				return;
			}
			switch (option.type) {
				case config_schema::ValueType::boolean:
					for (const auto value : kBooleanCompletionValues) {
						std::cout << value << '\n';
					}
					return;
				case config_schema::ValueType::integer:
				case config_schema::ValueType::floating_point:
				case config_schema::ValueType::string:
					return;
			}
		}

		auto print_completion_command_values(std::string_view command_name, std::size_t position,
		                                     std::span<const std::string> previous_positionals)
		    -> bool {
			const auto *descriptor = find_command(command_name);
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
				for (const auto &option : config_schema::runtime_config_options()) {
					std::cout << option.key << '\n';
				}
				return true;
			}
			if (position != 1) {
				return false;
			}
			const auto &key = previous_positionals.front();
			for (const auto &option : config_schema::runtime_config_options()) {
				if (option.key == key) {
					print_config_option_values(option);
					break;
				}
			}
			return true;
		}

	}  // namespace

	auto handle_completion_query(const std::vector<std::string> &arguments, bool global_option_seen)
	    -> int {
		if (global_option_seen) {
			return 1;
		}
		if (!arguments.empty() && arguments.front() == "commands") {
			return print_completion_commands(std::span<const std::string>(arguments).subspan(1))
			           ? 0
			           : 1;
		}
		if (arguments.size() == 1 && arguments.front() == "global-options") {
			print_completion_global_options();
			return 0;
		}
		if (arguments.size() == 2 && arguments.front() == "command-options") {
			print_completion_command_options(arguments[1]);
			return find_command(arguments[1]) == nullptr ? 1 : 0;
		}
		if (arguments.size() == 2 && arguments.front() == "command-max-positionals") {
			print_completion_command_max_positionals(arguments[1]);
			return find_command(arguments[1]) == nullptr ? 1 : 0;
		}
		if (arguments.size() >= 3 && arguments.front() == "command-values") {
			std::size_t position = 0;
			if (parse_completion_position(arguments[2], position) &&
			    position == arguments.size() - 3 &&
			    print_completion_command_values(
			        arguments[1], position, std::span<const std::string>(arguments).subspan(3))) {
				return 0;
			}
		}
		return 1;
	}

}  // namespace howdy::native::howdy_completion_internal
