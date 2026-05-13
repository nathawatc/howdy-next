#include "howdy/config/config_utils.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace howdy::native {

auto read_config_lines(const std::filesystem::path &config_path, bool lock)
    -> std::vector<std::string> {
  std::vector<std::string> lines;

  const int fd = open(config_path.c_str(), O_RDONLY);
  if (fd < 0) {
    return lines;
  }

  if (lock) {
    flock(fd, LOCK_EX);
  }

  std::ifstream input(config_path);
  std::string line;
  while (std::getline(input, line)) {
    lines.push_back(line + "\n");
  }

  if (lock) {
    flock(fd, LOCK_UN);
  }
  close(fd);
  return lines;
}

auto atomic_write_lines(const std::filesystem::path &config_path,
                        const std::vector<std::string> &lines) -> bool {
  const auto parent = config_path.parent_path();
  std::filesystem::create_directories(parent);

  std::string temp = (parent / ".howdy-config-XXXXXX").string();
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
      for (const auto &line : lines) {
        output << line;
      }
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
  std::filesystem::rename(temp_path, config_path, ec);
  if (ec) {
    std::filesystem::remove(temp_path, ec);
    return false;
  }
  return true;
}

auto update_config_value(const std::filesystem::path &config_path,
                         const std::string &key, const std::string &value,
                         bool lock) -> bool {
  auto lines = read_config_lines(config_path, lock);
  for (auto &line : lines) {
    const auto stripped_pos = line.find_first_not_of(" \t");
    if (stripped_pos == std::string::npos) {
      continue;
    }

    const auto stripped = line.substr(stripped_pos);
    if (stripped.rfind(key + " =", 0) == 0 || stripped.rfind(key + " ", 0) == 0) {
      line = key + " = " + value + "\n";
      return atomic_write_lines(config_path, lines);
    }
  }

  return false;
}

}  // namespace howdy::native
