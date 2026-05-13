#include <pwd.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace {

auto resolve_user() -> std::string {
  for (const char *name : {"SUDO_USER", "DOAS_USER"}) {
    if (const char *value = std::getenv(name); value != nullptr && value[0] != '\0') {
      return value;
    }
  }

  if (const char *pkexec_uid = std::getenv("PKEXEC_UID");
      pkexec_uid != nullptr && pkexec_uid[0] != '\0') {
    const auto uid = static_cast<uid_t>(std::stoi(pkexec_uid));
    if (passwd *pwd = getpwuid(uid); pwd != nullptr) {
      return std::string(pwd->pw_name);
    }
  }

  if (passwd *pwd = getpwuid(getuid()); pwd != nullptr) {
    return pwd->pw_name;
  }
  return {};
}

auto find_binary(const std::string &binary_name) -> std::string {
  for (const char *libdir : {"/usr/lib/howdy", "/usr/lib64/howdy",
                             "/usr/local/lib/howdy"}) {
    const auto candidate = std::filesystem::path(libdir) / binary_name;
    if (std::filesystem::is_regular_file(candidate) &&
        access(candidate.c_str(), X_OK) == 0) {
      return candidate.string();
    }
  }
  return {};
}

void print_help(const std::string &user) {
  std::cout << "current active user: " << user << "\n\n";
  std::cout << "usage: howdy [-U USER] [--plain] [-h] [-y] {command} [arguments...]\n";
}

}  // namespace

int main(int argc, char *argv[]) {
  const std::string default_user = resolve_user();
  if (default_user.empty()) {
    std::cout << "Could not determine user, please use the --user flag\n";
    return 1;
  }

  std::string user = default_user;
  bool yes = false;
  bool plain = false;
  std::string command;
  std::vector<std::string> arguments;

  for (int index = 1; index < argc; ++index) {
    const std::string_view arg(argv[index]);
    if (arg == "-U" || arg == "--user") {
      if (index + 1 < argc) {
        user = argv[++index];
      }
      continue;
    }
    if (arg == "-y") {
      yes = true;
      continue;
    }
    if (arg == "--plain") {
      plain = true;
      continue;
    }
    if (arg == "-h" || arg == "--help") {
      print_help(user);
      return 0;
    }
    if (command.empty()) {
      command = argv[index];
      continue;
    }
    arguments.emplace_back(argv[index]);
  }

  if (command.empty()) {
    print_help(user);
    return 0;
  }

  if (geteuid() != 0) {
    std::cout << "Please run this command as root:\n\n";
    std::cout << "\tsudo howdy";
    for (int index = 1; index < argc; ++index) {
      std::cout << " " << argv[index];
    }
    std::cout << "\n";
    return 1;
  }

  if (user == "root") {
    std::cout << "Can't run howdy commands as root, please run this command with the --user flag\n";
    return 1;
  }

  const std::map<std::string, std::string> native_commands = {
      {"add", "howdy-add"},
      {"clear", "howdy-clear"},
      {"config", "howdy-config"},
      {"disable", "howdy-disable"},
      {"download-models", "howdy-download-models"},
      {"list", "howdy-list"},
      {"remove", "howdy-remove"},
      {"set", "howdy-set"},
      {"snapshot", "howdy-snapshot"},
      {"test", "howdy-test"},
  };

  if (command == "version") {
    std::cout << "Howdy-Next 1.0.0\n";
    return 0;
  }

  const auto it = native_commands.find(command);
  if (it == native_commands.end()) {
    std::cout << "Unknown command: " << command << "\n";
    return 1;
  }

  const auto binary_path = find_binary(it->second);
  if (binary_path.empty()) {
    std::cout << "Missing native command binary: " << it->second << "\n";
    return 1;
  }

  std::vector<std::string> argv_strings;
  argv_strings.push_back(binary_path);
  if (command != "snapshot" && command != "config" && command != "download-models") {
    argv_strings.push_back(user);
  }
  argv_strings.insert(argv_strings.end(), arguments.begin(), arguments.end());
  if (plain) {
    argv_strings.emplace_back("--plain");
  }
  if (yes) {
    argv_strings.emplace_back("-y");
  }

  std::vector<char *> exec_argv;
  exec_argv.reserve(argv_strings.size() + 1);
  for (auto &value : argv_strings) {
    exec_argv.push_back(value.data());
  }
  exec_argv.push_back(nullptr);

  execv(binary_path.c_str(), exec_argv.data());
  std::cout << "Failed to execute " << binary_path << "\n";
  return 1;
}
