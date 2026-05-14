#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace howdy::native {

inline constexpr auto kInvalidUserNameMessage =
    "Invalid user name. Refusing to use an unsafe model file path.";

inline auto is_valid_model_user_name(const std::string_view user) -> bool {
  if (user.empty() || user == "." || user == ".." || user.size() > 255) {
    return false;
  }

  if (user.front() == '.') {
    return false;
  }

  return std::all_of(
      user.begin(), user.end(),
      [](const char raw) {
        const auto ch = static_cast<unsigned char>(raw);
        return raw != '/' && raw != '\\' && std::isspace(ch) == 0 &&
               std::iscntrl(ch) == 0;
      });
}

inline auto resolve_user_model_path(const std::filesystem::path &base_dir,
                                    const std::string_view user)
    -> std::optional<std::filesystem::path> {
  if (!is_valid_model_user_name(user)) {
    return std::nullopt;
  }

  return base_dir / (std::string(user) + ".dat");
}

}  // namespace howdy::native
