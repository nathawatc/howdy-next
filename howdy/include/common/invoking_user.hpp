#pragma once

#include <cerrno>
#include <cstdlib>
#include <limits>
#include <optional>
#include <pwd.h>
#include <string>

namespace howdy::native {

struct InvokingUser {
  uid_t uid = 0;
  gid_t gid = 0;
  std::string name;
  std::string home;
  std::string shell;
};

inline auto parse_uid_env(const char *value) -> std::optional<uid_t> {
  if (value == nullptr || value[0] == '\0') {
    return std::nullopt;
  }

  errno = 0;
  char *end = nullptr;
  const auto raw_uid = std::strtoul(value, &end, 10);
  if (errno != 0 || end == value || end == nullptr || *end != '\0' ||
      raw_uid > std::numeric_limits<uid_t>::max()) {
    return std::nullopt;
  }

  return static_cast<uid_t>(raw_uid);
}

inline auto parse_gid_env(const char *value) -> std::optional<gid_t> {
  if (value == nullptr || value[0] == '\0') {
    return std::nullopt;
  }

  errno = 0;
  char *end = nullptr;
  const auto raw_gid = std::strtoul(value, &end, 10);
  if (errno != 0 || end == value || end == nullptr || *end != '\0' ||
      raw_gid > std::numeric_limits<gid_t>::max()) {
    return std::nullopt;
  }

  return static_cast<gid_t>(raw_gid);
}

inline auto invoking_user_from_pwd(const passwd &pwd, gid_t gid_override)
    -> InvokingUser {
  return InvokingUser{
      .uid = pwd.pw_uid,
      .gid = gid_override,
      .name = pwd.pw_name,
      .home = pwd.pw_dir != nullptr ? pwd.pw_dir : "",
      .shell = pwd.pw_shell != nullptr ? pwd.pw_shell : "",
  };
}

inline auto resolve_invoking_user() -> std::optional<InvokingUser> {
  if (const auto sudo_uid = parse_uid_env(std::getenv("SUDO_UID"))) {
    if (passwd *pwd = getpwuid(*sudo_uid); pwd != nullptr) {
      const auto sudo_gid =
          parse_gid_env(std::getenv("SUDO_GID")).value_or(pwd->pw_gid);
      return invoking_user_from_pwd(*pwd, sudo_gid);
    }
  }

  if (const char *doas_user = std::getenv("DOAS_USER");
      doas_user != nullptr && doas_user[0] != '\0') {
    if (passwd *pwd = getpwnam(doas_user); pwd != nullptr) {
      return invoking_user_from_pwd(*pwd, pwd->pw_gid);
    }
  }

  if (const auto pkexec_uid = parse_uid_env(std::getenv("PKEXEC_UID"))) {
    if (passwd *pwd = getpwuid(*pkexec_uid); pwd != nullptr) {
      return invoking_user_from_pwd(*pwd, pwd->pw_gid);
    }
  }

  return std::nullopt;
}

}  // namespace howdy::native
