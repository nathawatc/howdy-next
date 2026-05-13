#include "cli/remove_cli.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "config/runtime_paths.hpp"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitAbort = 1;

struct RemoveArgs {
  std::string user;
  std::string id;
  bool yes = false;
};

auto parse_args(int argc, char *argv[]) -> RemoveArgs {
  RemoveArgs args;
  if (argc < 2) {
    std::exit(kExitAbort);
  }
  args.user = argv[1];
  for (int index = 2; index < argc; ++index) {
    const std::string_view arg(argv[index]);
    if (arg == "-y") {
      args.yes = true;
      continue;
    }
    if (args.id.empty()) {
      args.id = argv[index];
    }
  }
  return args;
}

auto save_models_atomic(const std::filesystem::path &path,
                        const nlohmann::json &models) -> bool {
  const auto parent = path.parent_path();
  std::string temp = (parent / ".howdy-models-XXXXXX").string();
  std::vector<char> writable(temp.begin(), temp.end());
  writable.push_back('\0');

  const int fd = mkstemp(writable.data());
  if (fd < 0) {
    return false;
  }

  const std::filesystem::path temp_path(writable.data());
  bool ok = false;
  {
    std::ofstream output(temp_path);
    if (output.is_open()) {
      output << models.dump();
      ok = output.good();
    }
  }
  close(fd);

  if (!ok) {
    std::error_code ec;
    std::filesystem::remove(temp_path, ec);
    return false;
  }

  std::error_code ec;
  std::filesystem::rename(temp_path, path, ec);
  if (ec) {
    std::filesystem::remove(temp_path, ec);
    return false;
  }
  return true;
}

}  // namespace

int remove_main(int argc, char *argv[]) {
  const auto args = parse_args(argc, argv);
  if (args.id.empty()) {
    std::cout << "Please add the ID of the model you want to remove as an argument\n";
    std::cout << "For example:\n";
    std::cout << "\n\thowdy remove 0\n\n";
    std::cout << "You can find the IDs by running:\n";
    std::cout << "\n\thowdy list\n\n";
    return kExitAbort;
  }

  const auto models_dir = howdy::native::resolve_user_models_dir();
  if (!std::filesystem::exists(models_dir)) {
    std::cout << "Face models have not been initialized yet, please run:\n";
    std::cout << "\n\thowdy add\n\n";
    return kExitAbort;
  }

  const auto model_path = models_dir / (args.user + ".dat");
  std::ifstream input(model_path);
  if (!input.is_open()) {
    std::cout << "No face model known for the user " << args.user << ", please run:\n";
    std::cout << "\n\thowdy add\n\n";
    return kExitAbort;
  }

  nlohmann::json models;
  input >> models;

  int found_index = -1;
  std::string found_label;
  for (std::size_t index = 0; index < models.size(); ++index) {
    if (std::to_string(models[index].value("id", -1)) == args.id) {
      found_index = static_cast<int>(index);
      found_label = models[index].value("label", std::string());
      break;
    }
  }

  if (found_index < 0) {
    std::cout << "No model with ID " << args.id << " exists for " << args.user
              << "\n";
    return kExitAbort;
  }

  if (!args.yes) {
    std::cout << "This will remove the model called \"" << found_label
              << "\" for " << args.user << "\n";
    std::cout << "Do you want to continue [y/N]: ";
    std::string answer;
    std::getline(std::cin, answer);
    if (answer != "y" && answer != "Y") {
      std::cout << "\nInterpreting as a \"NO\", aborting\n";
      return kExitAbort;
    }
    std::cout << "\n";
  }

  if (models.size() == 1) {
    std::filesystem::remove(model_path);
    std::cout << "Removed last model, howdy disabled for user\n";
    return kExitOk;
  }

  models.erase(models.begin() + found_index);
  if (!save_models_atomic(model_path, models)) {
    std::cout << "Failed to update model file\n";
    return kExitAbort;
  }

  std::cout << "Removed model " << args.id << "\n";
  return kExitOk;
}
