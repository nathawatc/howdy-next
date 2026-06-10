#include "cli/list_cli.hpp"
#include "storage/user_models.hpp"

#include <algorithm>
#include <array>
#include <ctime>
#include <iostream>
#include <string>
#include <string_view>

namespace {

	constexpr int kExitOk    = 0;
	constexpr int kExitAbort = 1;

	struct ListArgs {
		std::string user;
		bool        plain = false;
	};

	auto parse_args(int argc, char **argv) -> ListArgs {
		ListArgs args;
		if (argc < 2) {
			std::exit(kExitAbort);
		}
		args.user = argv[1];
		for (int index = 2; index < argc; ++index) {
			if (std::string_view(argv[index]) == "--plain") {
				args.plain = true;
			}
		}
		return args;
	}

}  // namespace

int list_main(int argc, char **argv) {
	const auto args   = parse_args(argc, argv);
	const auto models = howdy::native::list_user_model_entries(args.user, {});
	if (models.status == howdy::native::UserModelStatus::kNoModelDirectory) {
		std::cout << "Face models have not been initialized yet, please run:\n";
		std::cout << "\n\tsudo howdy -U " << args.user << " add\n\n";
		return kExitAbort;
	}
	if (models.status == howdy::native::UserModelStatus::kNoModel) {
		if (!args.plain) {
			std::cout << "No face model known for the user " << args.user << ", please run:\n";
			std::cout << "\n\tsudo howdy -U " << args.user << " add\n\n";
		}
		return kExitAbort;
	}
	if (models.status != howdy::native::UserModelStatus::kOk) {
		if (!args.plain) {
			std::cout << models.error_message << "\n";
		}
		return kExitAbort;
	}
	for (const auto &model : models.entries) {
		const auto timestamp = static_cast<std::time_t>(model.time);
		std::cout << model.id;
		if (args.plain) {
			std::cout << ",";
		} else {
			std::cout << std::string(
			    std::max(0, 4 - static_cast<int>(std::to_string(model.id).size())), ' ');
		}
		std::array<char, 32> buffer{};
		std::strftime(buffer.data(), buffer.size(), "%Y-%m-%d %H:%M:%S",
		              std::localtime(&timestamp));
		std::cout << buffer.data();
		std::cout << (args.plain ? "," : "  ");
		std::cout << model.label << "\n";
	}

	std::cout << "\n";
	return kExitOk;
}
