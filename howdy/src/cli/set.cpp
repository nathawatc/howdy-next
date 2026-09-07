#include "cli/set.hpp"

#include "cli/set/internal.hpp"
#include "config/config_utils.hpp"
#include "config/runtime_paths.hpp"

#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

	constexpr int kSetExitOk    = 0;
	constexpr int kSetExitAbort = 1;

	auto SetCliResolveConfigPathDependency([[maybe_unused]] void *context)
	    -> std::filesystem::path {
		return howdy::native::ResolveConfigPath();
	}

	auto UpdateConfigValueDependency([[maybe_unused]] void       *context,
	                                 const std::filesystem::path &config_path,
	                                 const std::string &key, const std::string &value,
	                                 std::string *error_message, bool lock) -> bool {
		return howdy::native::UpdateConfigValue(config_path, key, error_message, value, lock);
	}

	struct SetArgs {
		std::string key;
		std::string value;
	};

	auto ParseSetArgs(int argc, char **argv) -> std::optional<SetArgs> {
		SetArgs     args;
		std::size_t positional_count = 0;
		bool        options_ended    = false;
		for (int index = 1; index < argc; ++index) {
			const std::string_view value(argv[index]);
			if (!options_ended && value == "--") {
				options_ended = true;
				continue;
			}
			if (!options_ended && !value.empty() && value.front() == '-') {
				return std::nullopt;
			}
			if (positional_count == 0) {
				args.key = value;
			} else if (positional_count == 1) {
				args.value = value;
			} else {
				return std::nullopt;
			}
			++positional_count;
		}
		return positional_count == 2 ? std::optional<SetArgs>{std::move(args)} : std::nullopt;
	}

}  // namespace

auto howdy::native::set_internal::SetMainWithDependencies(int argc, char **argv,
                                                          const SetDependencies &dependencies)
    -> int {
	if (argc < 3) {
		std::cout << "Please specify a setting and value.\n";
		std::cout << "For example:\n";
		std::cout << "\n\thowdy set sface_threshold 0.363\n\n";
		return kSetExitAbort;
	}
	const auto args = ParseSetArgs(argc, argv);
	if (!args.has_value()) {
		std::cout << "Invalid arguments for set\n";
		return kSetExitAbort;
	}
	if (dependencies.resolve_config_path == nullptr ||
	    dependencies.update_config_value == nullptr) {
		return kSetExitAbort;
	}

	const auto &config_path = dependencies.resolve_config_path(dependencies.context);
	const auto &key         = args->key;
	const auto &value       = args->value;
	if (!howdy::native::IsSafeIniScalarValue(value)) {
		std::cout << "Config values must be single-line scalars and cannot start with [\n";
		return kSetExitAbort;
	}
	std::string error_message;
	if (!dependencies.update_config_value(dependencies.context, config_path, key, value,
	                                      &error_message, true)) {
		std::cout << (error_message.empty() ? "Failed to update config option" : error_message)
		          << "\n";
		return kSetExitAbort;
	}

	std::cout << "Config option updated\n";
	return kSetExitOk;
}

auto SetMain(int argc, char **argv) -> int {
	return howdy::native::set_internal::SetMainWithDependencies(
	    argc, argv,
	    howdy::native::set_internal::SetDependencies{
	        .resolve_config_path = SetCliResolveConfigPathDependency,
	        .update_config_value = UpdateConfigValueDependency,
	    });
}
