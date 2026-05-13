#include "howdy/cli/clear_cli.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#include "howdy/config/runtime_paths.hpp"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitAbort = 1;

struct ClearArgs {
  std::string user;
  bool yes = false;
};

auto parse_args(int argc, char *argv[]) -> ClearArgs {
  ClearArgs args;
  if (argc < 2) {
    std::exit(kExitAbort);
  }
  args.user = argv[1];
  for (int index = 2; index < argc; ++index) {
    if (std::string_view(argv[index]) == "-y") {
      args.yes = true;
    }
  }
  return args;
}

}  // namespace

int clear_main(int argc, char *argv[]) {
  const auto args = parse_args(argc, argv);
  const auto models_dir = howdy::native::resolve_user_models_dir();
  if (!std::filesystem::exists(models_dir)) {
    std::cout << "No models created yet, can't clear them if they don't exist\n";
    return kExitAbort;
  }

  const auto model_path = models_dir / (args.user + ".dat");
  if (!std::filesystem::is_regular_file(model_path)) {
    std::cout << args.user << " has no models or they have been cleared already\n";
    return kExitAbort;
  }

  if (!args.yes) {
    std::cout << "This will clear all models for " << args.user << "\n";
    std::cout << "Do you want to continue [y/N]: ";
    std::string answer;
    std::getline(std::cin, answer);
    if (answer != "y" && answer != "Y") {
      std::cout << "\nInterpreting as a \"NO\", aborting\n";
      return kExitAbort;
    }
  }

  std::filesystem::remove(model_path);
  std::cout << "\nModels cleared\n";
  return kExitOk;
}
