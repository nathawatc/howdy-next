#include "cli/set_cli.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "config/config_utils.hpp"
#include "config/runtime_paths.hpp"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitAbort = 1;

}  // namespace

int set_main(int argc, char **argv) {
  if (argc < 3) {
    std::cout << "Please add a setting you would like to change and the value to set it to\n";
    std::cout << "For example:\n";
    std::cout << "\n\thowdy set sface_threshold 0.363\n\n";
    return kExitAbort;
  }

  const auto config_path = howdy::native::resolve_config_path();
  const std::string key = argv[1];
  const std::string value = argv[2];
  if (!howdy::native::update_config_value(config_path, key, value)) {
    std::cout << "Could not find a \"" << key << "\" config option to set\n";
    return kExitAbort;
  }

  std::cout << "Config option updated\n";
  return kExitOk;
}
