#include "cli/list_cli.hpp"

#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "config/runtime_paths.hpp"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitAbort = 1;

struct ListArgs {
  std::string user;
  bool plain = false;
};

auto parse_args(int argc, char *argv[]) -> ListArgs {
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

int list_main(int argc, char *argv[]) {
  const auto args = parse_args(argc, argv);
  const auto models_dir = howdy::native::resolve_user_models_dir();
  if (!std::filesystem::exists(models_dir)) {
    std::cout << "Face models have not been initialized yet, please run:\n";
    std::cout << "\n\tsudo howdy -U " << args.user << " add\n\n";
    return kExitAbort;
  }

  const auto model_path = models_dir / (args.user + ".dat");
  std::ifstream input(model_path);
  if (!input.is_open()) {
    if (!args.plain) {
      std::cout << "No face model known for the user " << args.user
                << ", please run:\n";
      std::cout << "\n\tsudo howdy -U " << args.user << " add\n\n";
    }
    return kExitAbort;
  }

  nlohmann::json models;
  input >> models;
  for (const auto &model : models) {
    const int id = model.value("id", -1);
    const auto timestamp = static_cast<std::time_t>(model.value("time", 0LL));
    std::cout << id;
    if (args.plain) {
      std::cout << ",";
    } else {
      std::cout << std::string(std::max(0, 4 - static_cast<int>(std::to_string(id).size())), ' ');
    }
    char buffer[32] = {0};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S",
                  std::localtime(&timestamp));
    std::cout << buffer;
    std::cout << (args.plain ? "," : "  ");
    std::cout << model.value("label", std::string()) << "\n";
  }

  std::cout << "\n";
  return kExitOk;
}
