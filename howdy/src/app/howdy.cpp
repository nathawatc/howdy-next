#include "app/command_catalog.hpp"
#include "app/howdy/internal.hpp"
#include "howdy/cli/internal.hpp"
#include "howdy/completion/internal.hpp"
#include "howdy/version_format.hpp"
#include "support/user_names.hpp"
#include "version.hpp"

#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

	using howdy::native::CommandInvocation;
	using howdy::native::howdy_cli_internal::CliSyntaxError;
	using howdy::native::howdy_cli_internal::CliSyntaxErrorKind;
	using howdy::native::howdy_cli_internal::ParsedCommandLine;
	using howdy::native::howdy_internal::HowdyDependencies;

	auto HandleSpecialCommand(const ParsedCommandLine &parsed) -> std::optional<int> {
		if (!parsed.command.has_value()) {
			howdy::native::howdy_cli_internal::PrintHelp();
			return 0;
		}
		if (parsed.help_requested && !parsed.help_after_command) {
			howdy::native::howdy_cli_internal::PrintHelp();
			return 0;
		}
		if (*parsed.command != "__complete") {
			return std::nullopt;
		}
		std::vector<std::string> completion_arguments;
		completion_arguments.reserve(parsed.arguments.size());
		for (const auto &argument : parsed.arguments) {
			completion_arguments.push_back(argument.value);
		}
		return howdy::native::howdy_completion_internal::HandleCompletionQuery(
		    completion_arguments, parsed.global_option_seen);
	}

	auto ResolveModelUser(CommandInvocation &invocation, const HowdyDependencies &dependencies)
	    -> bool {
		if (!invocation.resolved_user.has_value()) {
			if (dependencies.resolve_invoking_identity == nullptr) {
				return false;
			}

			const auto identity = dependencies.resolve_invoking_identity(dependencies.context);
			switch (identity.status) {
				case howdy::native::InvokingIdentityStatus::kNoWrapperIdentity:
					std::cout << "Unable to determine the user; please use --user\n";
					return false;
				case howdy::native::InvokingIdentityStatus::kResolved:
					if (!identity.user.has_value()) {
						std::cout
						    << "Unable to determine the user: invalid invoking identity; please "
						       "use --user\n";
						return false;
					}
					invocation.resolved_user = identity.user->name;
					break;
				case howdy::native::InvokingIdentityStatus::kInvalid:
					std::cout
					    << "Unable to determine the user: invalid privilege-wrapper identity; "
					       "please use --user\n";
					return false;
				case howdy::native::InvokingIdentityStatus::kConflicting:
					std::cout
					    << "Unable to determine the user: conflicting privilege-wrapper identity; "
					       "please use --user\n";
					return false;
			}
		}
		if (!invocation.resolved_user.has_value() || invocation.resolved_user->empty()) {
			std::cout << "Unable to determine the user; please use --user\n";
			return false;
		}
		if (!howdy::native::IsValidModelUserName(*invocation.resolved_user)) {
			std::cout << howdy::native::kInvalidUserNameMessage << "\n";
			return false;
		}
		return true;
	}

}  // namespace

auto howdy::native::howdy_internal::HowdyMainWithDependencies(int argc, char **argv,
                                                              const HowdyDependencies &dependencies)
    -> int {
	if (const auto error =
	        howdy::native::ValidateCommandCatalog(howdy::native::CommandCatalog(), true)) {
		std::cerr << "howdy: command catalog validation failed: " << *error << '\n';
		return 1;
	}
	if (const auto error = howdy::native::ValidateGlobalOptionCatalog(
	        howdy::native::GlobalOptionCatalog(), true)) {
		std::cerr << "howdy: global option catalog validation failed: " << *error << '\n';
		return 1;
	}

	ParsedCommandLine parsed;
	if (const auto parse_error =
	        howdy::native::howdy_cli_internal::ParseCommandLine(argc, argv, parsed);
	    parse_error.has_value()) {
		const auto *command =
		    parsed.command.has_value() ? howdy::native::FindCommand(*parsed.command) : nullptr;
		return howdy::native::howdy_cli_internal::PrintCliSyntaxError(*parse_error, command);
	}

	if (const auto special_result = HandleSpecialCommand(parsed); special_result.has_value()) {
		return *special_result;
	}
	if (!parsed.command.has_value()) {
		return 1;
	}
	const auto &command_name       = parsed.command.value();
	const auto *command_descriptor = howdy::native::FindCommand(command_name);
	if (command_descriptor == nullptr) {
		return howdy::native::howdy_cli_internal::PrintCliSyntaxError(
		    CliSyntaxError{
		        .kind  = CliSyntaxErrorKind::kUnrecognizedSubcommand,
		        .value = command_name,
		    },
		    nullptr);
	}
	if (parsed.help_requested && parsed.help_after_command) {
		howdy::native::howdy_cli_internal::PrintCommandHelp(*command_descriptor);
		return 0;
	}
	auto invocation_result =
	    howdy::native::howdy_cli_internal::ParseCommandInvocation(parsed, *command_descriptor);
	if (!invocation_result.has_value()) {
		return howdy::native::howdy_cli_internal::PrintCliSyntaxError(invocation_result.error(),
		                                                              command_descriptor);
	}
	auto invocation = std::move(invocation_result.value());
	if (command_descriptor->kind == howdy::native::CommandKind::kVersion) {
		std::cout << howdy::native::FormatVersion(howdy::native::kProjectVersion,
		                                          howdy::native::kBuildCommit)
		          << "\n";
		return 0;
	}

	const bool needs_user_argument =
	    command_descriptor->user_target == howdy::native::UserTargetMode::kModelUser;
	if (dependencies.effective_uid == nullptr ||
	    dependencies.effective_uid(dependencies.context) != 0) {
		std::cout << "This command requires root privileges.\n";
		std::cout << "Run it again with sudo.\n";
		return 1;
	}
	if (needs_user_argument && !ResolveModelUser(invocation, dependencies)) {
		return 1;
	}

	const auto selected_main =
	    dependencies.command_mains[static_cast<std::size_t>(command_descriptor->id)];
	if (selected_main == nullptr) {
		std::cerr << "howdy: command entrypoint unavailable: " << command_name << '\n';
		return 1;
	}

	return selected_main(invocation);
}
