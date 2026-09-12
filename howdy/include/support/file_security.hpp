#pragma once

#include "support/file_security/validation_root.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>

#include <sys/stat.h>

namespace howdy::native {

	enum class SecurePathKind : std::uint8_t {
		kRegularFile,
		kDirectory,
	};

	struct SecurePathCheckResult {
		bool        ok = false;
		std::string error_message;
		int         error_code = 0;
	};

	inline auto DefaultSecureOwnerUid() -> std::optional<uid_t> {
		if (geteuid() == 0) {
			return static_cast<uid_t>(0);
		}
		return std::nullopt;
	}

	inline auto SecurePathKindName(SecurePathKind kind) -> const char * {
		switch (kind) {
			case SecurePathKind::kRegularFile:
				return "regular file";
			case SecurePathKind::kDirectory:
				return "directory";
		}
		return "path";
	}

	inline auto CheckSecurePathStat(const struct stat &stat, SecurePathKind kind,
	                                const std::filesystem::path &path, const std::string_view label,
	                                const std::optional<uid_t> owner_uid = DefaultSecureOwnerUid())
	    -> SecurePathCheckResult {
		const bool type_ok =
		    kind == SecurePathKind::kRegularFile ? S_ISREG(stat.st_mode) : S_ISDIR(stat.st_mode);
		if (!type_ok) {
			return SecurePathCheckResult{
			    .ok            = false,
			    .error_message = std::string(label) + " must be a " + SecurePathKindName(kind) +
			                     ": " + path.string(),
			    .error_code    = 0,
			};
		}

		if (owner_uid.has_value() && stat.st_uid != *owner_uid) {
			return SecurePathCheckResult{
			    .ok            = false,
			    .error_message = std::string(label) + " must be owned by UID " +
			                     std::to_string(*owner_uid) + ": " + path.string(),
			    .error_code    = 0,
			};
		}

		if ((stat.st_mode & S_IWGRP) != 0) {
			return SecurePathCheckResult{
			    .ok = false,
			    .error_message =
			        std::string(label) + " must not be group-writable: " + path.string(),
			    .error_code = 0,
			};
		}

		if ((stat.st_mode & S_IWOTH) != 0) {
			return SecurePathCheckResult{
			    .ok = false,
			    .error_message =
			        std::string(label) + " must not be world-writable: " + path.string(),
			    .error_code = 0,
			};
		}

		if (kind == SecurePathKind::kRegularFile && stat.st_nlink != 1) {
			return SecurePathCheckResult{
			    .ok            = false,
			    .error_message = std::string(label) + " must not be hard-linked: " + path.string(),
			    .error_code    = 0,
			};
		}

		return SecurePathCheckResult{.ok = true, .error_message = {}, .error_code = 0};
	}

	inline auto CheckSecureFd(int fd, SecurePathKind kind, const std::filesystem::path &path,
	                          const std::string_view     label,
	                          const std::optional<uid_t> owner_uid = DefaultSecureOwnerUid())
	    -> SecurePathCheckResult {
		struct stat stat{};
		if (fd < 0 || fstat(fd, &stat) != 0) {
			const int error_code = fd < 0 ? EBADF : errno;
			return SecurePathCheckResult{
			    .ok            = false,
			    .error_message = "Failed to inspect " + std::string(label) + ": " + path.string() +
			                     " (" + std::strerror(error_code) + ")",
			    .error_code    = error_code,
			};
		}
		return CheckSecurePathStat(stat, kind, path, label, owner_uid);
	}

	inline auto
	CheckSecureRootOwnedFd(int fd, const std::filesystem::path &path, const std::string_view label,
	                       const std::optional<uid_t> owner_uid = DefaultSecureOwnerUid())
	    -> SecurePathCheckResult {
		return CheckSecureFd(fd, SecurePathKind::kRegularFile, path, label, owner_uid);
	}

	inline auto CheckSecurePath(const std::filesystem::path &path, SecurePathKind kind,
	                            const std::string_view     label,
	                            const std::optional<uid_t> owner_uid = DefaultSecureOwnerUid())
	    -> SecurePathCheckResult {
		struct stat stat{};
		if (lstat(path.c_str(), &stat) != 0) {
			const int error_code = errno;
			return SecurePathCheckResult{
			    .ok            = false,
			    .error_message = "Failed to inspect " + std::string(label) + ": " + path.string() +
			                     " (" + std::strerror(error_code) + ")",
			    .error_code    = error_code,
			};
		}
		return CheckSecurePathStat(stat, kind, path, label, owner_uid);
	}

	inline auto
	CheckSecureRootOwnedFile(const std::filesystem::path &path, const std::string_view label,
	                         const std::optional<uid_t> owner_uid = DefaultSecureOwnerUid())
	    -> SecurePathCheckResult {
		return CheckSecurePath(path, SecurePathKind::kRegularFile, label, owner_uid);
	}

	inline auto
	CheckSecureRootOwnedDirectory(const std::filesystem::path &path, const std::string_view label,
	                              const std::optional<uid_t> owner_uid = DefaultSecureOwnerUid())
	    -> SecurePathCheckResult {
		return CheckSecurePath(path, SecurePathKind::kDirectory, label, owner_uid);
	}

	inline auto CheckSecureRootOwnedDirectoryTree(
	    const std::filesystem::path &path, const std::string_view label,
	    const std::optional<uid_t>                    owner_uid       = DefaultSecureOwnerUid(),
	    const file_security_internal::ValidationRoot &validation_root = {})
	    -> SecurePathCheckResult {
		if (!validation_root.path.empty()) {
			const auto relative = file_security_internal::RelativeTarget(validation_root, path);
			if (!relative) {
				return {.error_message =
				            std::string(label) + ": target outside validation boundary"};
			}
			auto current = validation_root.path;
			// Strip trailing separators/dots so lstat still rejects a symlink root.
			while (current != current.root_path() &&
			       (current.filename().empty() || current.filename() == ".")) {
				current = current.parent_path();
			}
			auto security = CheckSecureRootOwnedDirectory(current, label, owner_uid);
			if (!security.ok) {
				return security;
			}
			for (const auto &component : *relative) {
				if (component.empty() || component == ".") {
					continue;
				}
				current /= component;
				security = CheckSecureRootOwnedDirectory(current, label, owner_uid);
				if (!security.ok) {
					return security;
				}
			}
			return {.ok = true};
		}
		if (!path.is_absolute()) {
			return CheckSecureRootOwnedDirectory(path, label, owner_uid);
		}

		auto current = path.root_path();
		if (current.empty()) {
			current = "/";
		}

		auto root_security = CheckSecureRootOwnedDirectory(current, label, owner_uid);
		if (!root_security.ok) {
			return root_security;
		}

		const auto relative = path.lexically_relative(current);
		for (const auto &component : relative) {
			current /= component;
			const auto security = CheckSecureRootOwnedDirectory(current, label, owner_uid);
			if (!security.ok) {
				return security;
			}
		}

		return SecurePathCheckResult{.ok = true, .error_message = {}};
	}

	struct SecurePathLabels {
		std::string_view directory;
		std::string_view file;
	};

	inline auto CheckSecureRootOwnedFileWithDirectory(
	    const std::filesystem::path &path, const SecurePathLabels labels,
	    const std::optional<uid_t>                    owner_uid       = DefaultSecureOwnerUid(),
	    const file_security_internal::ValidationRoot &validation_root = {})
	    -> SecurePathCheckResult {
		const auto parent = path.parent_path();
		if (parent.empty()) {
			return SecurePathCheckResult{
			    .ok = false,
			    .error_message =
			        std::string(labels.file) + " must have a parent directory: " + path.string(),
			    .error_code = 0,
			};
		}

		auto directory_security =
		    CheckSecureRootOwnedDirectoryTree(parent, labels.directory, owner_uid, validation_root);
		if (!directory_security.ok) {
			return directory_security;
		}

		return CheckSecureRootOwnedFile(path, labels.file, owner_uid);
	}

	inline auto CheckSecureRootOwnedFdWithDirectory(
	    int fd, const std::filesystem::path &path, const SecurePathLabels labels,
	    const std::optional<uid_t>                    owner_uid       = DefaultSecureOwnerUid(),
	    const file_security_internal::ValidationRoot &validation_root = {})
	    -> SecurePathCheckResult {
		const auto parent = path.parent_path();
		if (parent.empty()) {
			return SecurePathCheckResult{
			    .ok = false,
			    .error_message =
			        std::string(labels.file) + " must have a parent directory: " + path.string(),
			    .error_code = 0,
			};
		}

		auto directory_security =
		    CheckSecureRootOwnedDirectoryTree(parent, labels.directory, owner_uid, validation_root);
		if (!directory_security.ok) {
			return directory_security;
		}

		return CheckSecureRootOwnedFd(fd, path, labels.file, owner_uid);
	}

}  // namespace howdy::native
