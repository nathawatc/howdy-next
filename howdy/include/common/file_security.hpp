#pragma once

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace howdy::native {

enum class SecurePathKind {
  kRegularFile,
  kDirectory,
};

struct SecurePathCheckResult {
  bool ok = false;
  std::string error_message;
};

inline auto default_secure_owner_uid() -> std::optional<uid_t> {
  if (geteuid() == 0) {
    return static_cast<uid_t>(0);
  }
  return std::nullopt;
}

inline auto secure_path_kind_name(SecurePathKind kind) -> const char * {
  switch (kind) {
  case SecurePathKind::kRegularFile:
    return "regular file";
  case SecurePathKind::kDirectory:
    return "directory";
  }
  return "path";
}

inline auto check_secure_path(
    const std::filesystem::path &path, SecurePathKind kind,
    const std::string_view label,
    const std::optional<uid_t> owner_uid = default_secure_owner_uid())
    -> SecurePathCheckResult {
  struct stat stat_ {};
  if (lstat(path.c_str(), &stat_) != 0) {
    return SecurePathCheckResult{
        .ok = false,
        .error_message = "Failed to inspect " + std::string(label) + ": " +
                         path.string() + " (" + std::strerror(errno) + ")",
    };
  }

  const bool type_ok =
      kind == SecurePathKind::kRegularFile ? S_ISREG(stat_.st_mode)
                                           : S_ISDIR(stat_.st_mode);
  if (!type_ok) {
    return SecurePathCheckResult{
        .ok = false,
        .error_message = std::string(label) + " must be a " +
                         secure_path_kind_name(kind) + ": " + path.string(),
    };
  }

  if (owner_uid.has_value() && stat_.st_uid != *owner_uid) {
    return SecurePathCheckResult{
        .ok = false,
        .error_message = std::string(label) + " must be owned by root: " +
                         path.string(),
    };
  }

  if ((stat_.st_mode & S_IWGRP) != 0) {
    return SecurePathCheckResult{
        .ok = false,
        .error_message =
            std::string(label) + " must not be group-writable: " + path.string(),
    };
  }

  if ((stat_.st_mode & S_IWOTH) != 0) {
    return SecurePathCheckResult{
        .ok = false,
        .error_message =
            std::string(label) + " must not be world-writable: " + path.string(),
    };
  }

  if (kind == SecurePathKind::kRegularFile && stat_.st_nlink != 1) {
    return SecurePathCheckResult{
        .ok = false,
        .error_message = std::string(label) + " must not be hard-linked: " +
                         path.string(),
    };
  }

  return SecurePathCheckResult{.ok = true, .error_message = {}};
}

inline auto check_secure_root_owned_file(const std::filesystem::path &path,
                                         const std::string_view label)
    -> SecurePathCheckResult {
  return check_secure_path(path, SecurePathKind::kRegularFile, label);
}

inline auto check_secure_root_owned_directory(const std::filesystem::path &path,
                                              const std::string_view label)
    -> SecurePathCheckResult {
  return check_secure_path(path, SecurePathKind::kDirectory, label);
}

inline auto check_secure_root_owned_directory_tree(
    const std::filesystem::path &path, const std::string_view label)
    -> SecurePathCheckResult {
  if (!path.is_absolute()) {
    return check_secure_root_owned_directory(path, label);
  }

  auto current = path.root_path();
  if (current.empty()) {
    current = "/";
  }

  const auto root_security =
      check_secure_root_owned_directory(current, label);
  if (!root_security.ok) {
    return root_security;
  }

  const auto relative = path.lexically_relative(current);
  for (const auto &component : relative) {
    current /= component;
    const auto security =
        check_secure_root_owned_directory(current, label);
    if (!security.ok) {
      return security;
    }
  }

  return SecurePathCheckResult{.ok = true, .error_message = {}};
}

inline auto check_secure_root_owned_file_with_directory(
    const std::filesystem::path &path, const std::string_view directory_label,
    const std::string_view file_label) -> SecurePathCheckResult {
  const auto parent = path.parent_path();
  if (parent.empty()) {
    return SecurePathCheckResult{
        .ok = false,
        .error_message = std::string(file_label) +
                         " must have a parent directory: " + path.string(),
    };
  }

  const auto directory_security =
      check_secure_root_owned_directory_tree(parent, directory_label);
  if (!directory_security.ok) {
    return directory_security;
  }

  return check_secure_root_owned_file(path, file_label);
}

}  // namespace howdy::native
