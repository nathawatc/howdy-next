#include "cli/list_cli.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "common/file_security.hpp"
#include "common/user_names.hpp"
#include "config/runtime_paths.hpp"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitAbort = 1;
constexpr std::uintmax_t kMaxModelFileBytes = 1024 * 1024;

struct ListArgs {
  std::string user;
  bool plain = false;
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
  const auto args = parse_args(argc, argv);
  const auto models_dir = howdy::native::resolve_user_models_dir();
  if (!std::filesystem::exists(models_dir)) {
    std::cout << "Face models have not been initialized yet, please run:\n";
    std::cout << "\n\tsudo howdy -U " << args.user << " add\n\n";
    return kExitAbort;
  }
  const auto dir_security = howdy::native::check_secure_root_owned_directory(
      models_dir, "User models directory");
  if (!dir_security.ok) {
    if (!args.plain) {
      std::cout << dir_security.error_message << "\n";
    }
    return kExitAbort;
  }

  const auto model_path = howdy::native::resolve_user_model_path(models_dir, args.user);
  if (!model_path) {
    if (!args.plain) {
      std::cout << howdy::native::kInvalidUserNameMessage << "\n";
    }
    return kExitAbort;
  }

  if (!std::filesystem::is_regular_file(*model_path)) {
    if (!args.plain) {
      std::cout << "No face model known for the user " << args.user
                << ", please run:\n";
      std::cout << "\n\tsudo howdy -U " << args.user << " add\n\n";
    }
    return kExitAbort;
  }

  const auto model_security =
      howdy::native::check_secure_root_owned_file(*model_path, "User model file");
  if (!model_security.ok) {
    if (!args.plain) {
      std::cout << model_security.error_message << "\n";
    }
    return kExitAbort;
  }

  std::ifstream input(*model_path);
  if (!input.is_open()) {
    return kExitAbort;
  }

  std::error_code size_ec;
  if (std::filesystem::file_size(*model_path, size_ec) > kMaxModelFileBytes ||
      size_ec) {
    if (!args.plain) {
      std::cout << "Model file is too large to process safely\n";
    }
    return kExitAbort;
  }

  nlohmann::json models;
  try {
    input >> models;
  } catch (const nlohmann::json::exception &) {
    if (!args.plain) {
      std::cout << "Failed to parse model file\n";
    }
    return kExitAbort;
  }
  for (const auto &model : models) {
    const int id = model.value("id", -1);
    const auto timestamp = static_cast<std::time_t>(model.value("time", 0LL));
    std::cout << id;
    if (args.plain) {
      std::cout << ",";
    } else {
      std::cout << std::string(std::max(0, 4 - static_cast<int>(std::to_string(id).size())), ' ');
    }
    std::array<char, 32> buffer{};
    std::strftime(buffer.data(), buffer.size(), "%Y-%m-%d %H:%M:%S",
                  std::localtime(&timestamp));
    std::cout << buffer.data();
    std::cout << (args.plain ? "," : "  ");
    std::cout << model.value("label", std::string()) << "\n";
  }

  std::cout << "\n";
  return kExitOk;
}
