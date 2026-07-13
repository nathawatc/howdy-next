#include "cli/list_cli.hpp"
#include "cli/list_internal.hpp"
#include "storage/user_models.hpp"

#include <algorithm>
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
		args.user = argv[1];
		for (int index = 2; index < argc; ++index) {
			if (std::string_view(argv[index]) == "--plain") {
				args.plain = true;
			}
		}
		return args;
	}

	auto list_user_model_entries_dependency([[maybe_unused]] void *context, const std::string &user)
	    -> howdy::native::UserModelListResult {
		return howdy::native::list_user_model_entries(user, {});
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
		std::cout << "Face models have not been initialized yet, please run:\n";
		std::cout << "\n\tsudo howdy -U " << args->user << " add\n\n";
		return kExitAbort;
	}
	if (models.status == howdy::native::UserModelStatus::kNoModel) {
		if (!args->plain) {
			std::cout << "No face model known for the user " << args->user << ", please run:\n";
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
			std::cout << std::string(
			    std::max(0, 4 - static_cast<int>(std::to_string(model.id).size())), ' ');
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
		std::cout << model.label << "\n";
	}

	std::cout << "\n";
	return kExitOk;
}

int list_main(int argc, char **argv) {
	if (argc < 2) {
		return kExitAbort;
	}
	return howdy::native::list_internal::list_main_with_dependencies(
	    argc, argv,
	    howdy::native::list_internal::ListDependencies{
	        .list_user_model_entries = list_user_model_entries_dependency,
	    });
}
