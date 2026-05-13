#include "cli/disable_cli.hpp"

#include <iostream>
#include <string>

#include "config/config_reader.hpp"
#include "config/config_utils.hpp"
#include "config/runtime_paths.hpp"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitAbort = 1;

}  // namespace

int disable_main(int argc, char **argv) {
  if (argc < 2) {
    std::cout << "Please add a 0 (enable) or a 1 (disable) as an argument\n";
    return kExitAbort;
  }

  const std::string argument = argv[1];
  std::string out_value;
  if (argument == "1" || argument == "true") {
    out_value = "true";
  } else if (argument == "0" || argument == "false") {
    out_value = "false";
  } else {
    std::cout << "Please only use 0 (enable) or 1 (disable) as an argument\n";
    return kExitAbort;
  }

  const auto config_path = howdy::native::resolve_config_path();
  howdy::native::ConfigReader config(config_path.string());
  if (!config.ok()) {
    std::cout << "Failed to read config file: " << config_path << "\n";
    return kExitAbort;
  }

  if (out_value == config.get("core", "disabled", "true")) {
    std::cout << "The disable option has already been set to " << out_value
              << "\n";
    return kExitAbort;
  }

  if (!howdy::native::update_config_value(config_path, "disabled", out_value, true)) {
    std::cout << "Could not find a \"disabled\" config option to set\n";
    return kExitAbort;
  }

  std::cout << (out_value == "true" ? "Howdy has been disabled\n"
                                     : "Howdy has been enabled\n");
  return kExitOk;
}
