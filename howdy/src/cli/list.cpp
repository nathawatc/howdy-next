#include "cli/list_cli.hpp"
#include "cli/list_internal.hpp"
#include "storage/user_models.hpp"

#include <array>
#include <ctime>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

	constexpr int kExitOk    = 0;
	constexpr int kExitAbort = 1;

	struct ListArgs {
		std::string user;
		bool        plain = false;
	};

	auto parse_args(int argc, char **argv) -> std::optional<ListArgs> {
		ListArgs args;
		if (argc < 2) {
			return std::nullopt;
		}
		args.user          = argv[1];
		bool options_ended = false;
		for (int index = 2; index < argc; ++index) {
			const std::string_view arg(argv[index]);
			if (!options_ended && arg == "--") {
				options_ended = true;
				continue;
			}
			if (!options_ended && arg == "--plain") {
				args.plain = true;
				continue;
			}
			return std::nullopt;
		}
		return args;
	}

	auto list_user_model_entries_dependency([[maybe_unused]] void *context, const std::string &user)
	    -> howdy::native::UserModelListResult {
		return howdy::native::list_user_model_entries(user, {});
	}

	auto csv_field(const std::string_view value) -> std::string {
		if (!value.contains(',') && !value.contains('"')) {
			return std::string(value);
		}
		std::string escaped;
		escaped.reserve(value.size() + 2);
		escaped.push_back('"');
		for (const char character : value) {
			if (character == '"') {
				escaped.push_back('"');
			}
			escaped.push_back(character);
		}
		escaped.push_back('"');
		return escaped;
	}

}  // namespace

auto howdy::native::list_internal::list_main_with_dependencies(int argc, char **argv,
                                                               const ListDependencies &dependencies)
    -> int {
	if (dependencies.list_user_model_entries == nullptr) {
		return kExitAbort;
	}

	const auto args = parse_args(argc, argv);
	if (!args.has_value()) {
		return kExitAbort;
	}
	const auto models = dependencies.list_user_model_entries(dependencies.context, args->user);
	if (models.status == howdy::native::UserModelStatus::kNoModelDirectory) {
		std::cout << "No face models found. Please run:\n";
		std::cout << "\n\tsudo howdy -U " << args->user << " add\n\n";
		return kExitAbort;
	}
	if (models.status == howdy::native::UserModelStatus::kNoModel) {
		if (!args->plain) {
			std::cout << "No face models found. Please run:\n";
			std::cout << "\n\tsudo howdy -U " << args->user << " add\n\n";
		}
		return kExitAbort;
	}
	if (models.status != howdy::native::UserModelStatus::kOk) {
		if (!args->plain) {
			std::cout << models.error_message << "\n";
		}
		return kExitAbort;
	}
	for (const auto &model : models.entries) {
		std::cout << model.id;
		if (args->plain) {
			std::cout << ",";
		} else {
			constexpr std::size_t kIdColumnWidth = 4;
			const auto            id_size        = std::to_string(model.id).size();
			std::cout << std::string(id_size < kIdColumnWidth ? kIdColumnWidth - id_size : 0, ' ');
		}
		std::array<char, 32> buffer{};
		std::tm              local_time{};
		bool                 valid_time = false;
		if (std::in_range<std::time_t>(model.time)) {
			const auto timestamp = static_cast<std::time_t>(model.time);
			valid_time =
			    ::localtime_r(&timestamp, &local_time) != nullptr &&
			    std::strftime(buffer.data(), buffer.size(), "%Y-%m-%d %H:%M:%S", &local_time) != 0;
		}
		std::cout << (valid_time ? buffer.data() : "invalid-time");
		std::cout << (args->plain ? "," : "  ");
		std::cout << (args->plain ? csv_field(model.label) : model.label) << "\n";
	}

	std::cout << "\n";
	return kExitOk;
}

auto list_main(int argc, char **argv) -> int {
	if (argc < 2) {
		return kExitAbort;
	}
	return howdy::native::list_internal::list_main_with_dependencies(
	    argc, argv,
	    howdy::native::list_internal::ListDependencies{
	        .list_user_model_entries = list_user_model_entries_dependency,
	    });
}
