#include "cli/config_cli.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>

#include "config/runtime_paths.hpp"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitAbort = 1;

auto resolve_editor() -> std::string {
  if (const char *editor = std::getenv("EDITOR")) {
    if (editor[0] != '\0') {
      return editor;
    }
  }
  for (const char *candidate : {"micro", "nano", "vi"}) {
    if (access(("/usr/bin/" + std::string(candidate)).c_str(), X_OK) == 0) {
      return candidate;
    }
  }
  return {};
}

}  // namespace

int config_main(int, char **) {
  const auto editor = resolve_editor();
  if (editor.empty()) {
    std::cout << "Error: Could not find a suitable text editor.\n";
    std::cout << "Please install 'micro', 'nano', or 'vi', or set the EDITOR environment variable.\n";
    std::cout << "If you are running this command with sudo, try 'sudo -E howdy config' to preserve your EDITOR variable.\n";
    return kExitAbort;
  }

  const auto config_path = howdy::native::resolve_config_path();
  std::cout << "Opening config.ini in " << std::filesystem::path(editor).filename().string()
            << "\n";
  execlp(editor.c_str(), editor.c_str(), config_path.c_str(), nullptr);
  std::cout << "Failed to open editor\n";
  return kExitAbort;
}
