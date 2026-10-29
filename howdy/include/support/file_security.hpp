#pragma once

#include "support/error_message.hpp"
#include "support/file_security/validation_root.hpp"
#include "support/scoped_fd.hpp"

#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <fcntl.h>
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
				                 " (" + howdy::native::SystemErrorMessage(error_code) + ")",
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
				                 " (" + howdy::native::SystemErrorMessage(error_code) + ")",
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
				return {
				    .error_message = std::string(label) + ": target outside validation boundary",
				};
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

	namespace file_security_internal {

		struct ResolvedDirectoryRoot {
			SecurePathCheckResult check;
			std::filesystem::path root;
			std::filesystem::path relative;
		};

		inline auto ResolveDirectoryRoot(const std::filesystem::path &path,
		                                 const std::string_view       label,
		                                 const ValidationRoot        &validation_root)
		    -> ResolvedDirectoryRoot {
			const auto normalized = path.lexically_normal();
			if (!normalized.is_absolute()) {
				return {
				    .check =
				        {
				            .ok = false,
				            .error_message =
				                std::string(label) + " must be absolute: " + path.string(),
				            .error_code = EINVAL,
				        },
				};
			}

			if (!validation_root.path.empty()) {
				const auto rel = RelativeTarget(validation_root, normalized);
				if (!rel) {
					return {
					    .check =
					        {
					            .ok = false,
					            .error_message =
					                std::string(label) + ": target outside validation boundary",
					            .error_code = 0,
					        },
					};
				}
				auto root = validation_root.path;
				while (root != root.root_path() &&
				       (root.filename().empty() || root.filename() == ".")) {
					root = root.parent_path();
				}
				return {
				    .check =
				        {
				            .ok = true,
				        },
				    .root     = std::move(root),
				    .relative = *rel,
				};
			}

			auto root = normalized.root_path();
			if (root.empty()) {
				root = "/";
			}
			auto relative = normalized.lexically_relative(root);
			return {
			    .check =
			        {
			            .ok = true,
			        },
			    .root     = std::move(root),
			    .relative = std::move(relative),
			};
		}

		inline auto EnterSecureDirectoryComponent(int current_fd, const std::filesystem::path &comp,
		                                          mode_t                       mode,
		                                          const std::filesystem::path &comp_path,
		                                          const std::string_view       label,
		                                          const std::optional<uid_t>   owner_uid,
		                                          const ValidationRoot        &validation_root)
		    -> std::pair<SecurePathCheckResult, ScopedFd> {
			constexpr int k_dir_open_flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
			ScopedFd      next(openat(current_fd, comp.c_str(), k_dir_open_flags));
			if (!next.Valid()) {
				const int open_error = errno;
				if (open_error != ENOENT) {
					auto security = CheckSecureRootOwnedDirectoryTree(comp_path, label, owner_uid,
					                                                  validation_root);
					if (!security.ok) {
						return {std::move(security), {}};
					}
					return {
					    {
					        .ok            = false,
					        .error_message = "Failed to open " + std::string(label) +
							                 " component: " + comp_path.string(),
					        .error_code    = open_error,
					    },
					    {},
					};
				}
				const bool created = mkdirat(current_fd, comp.c_str(), mode) == 0;
				if (!created && errno != EEXIST) {
					return {
					    {
					        .ok            = false,
					        .error_message = "Failed to create " + std::string(label) +
							                 " component: " + comp_path.string(),
					        .error_code    = errno,
					    },
					    {},
					};
				}
				next.Reset(openat(current_fd, comp.c_str(), k_dir_open_flags));
				// Only chmod a directory we created; a raced EEXIST is validated below, not
				// modified.
				if (!next.Valid() || (created && fchmod(next.Get(), mode) != 0)) {
					return {
					    {
					        .ok            = false,
					        .error_message = "Failed to secure " + std::string(label) +
							                 " component: " + comp_path.string(),
					        .error_code    = errno,
					    },
					    {},
					};
				}
			}

			struct stat comp_st{};
			if (fstat(next.Get(), &comp_st) != 0) {
				return {
				    {
				        .ok            = false,
				        .error_message = "Failed to stat " + std::string(label) +
						                 " component: " + comp_path.string(),
				        .error_code    = errno,
				    },
				    {},
				};
			}
			auto sec = CheckSecurePathStat(comp_st, SecurePathKind::kDirectory, comp_path, label,
			                               owner_uid);
			if (!sec.ok) {
				return {std::move(sec), {}};
			}
			return {{
			            .ok = true,
			        },
			        std::move(next)};
		}

	}  // namespace file_security_internal

	inline auto
	CreateSecureDirectoryTree(const std::filesystem::path &path, const std::string_view label,
	                          const mode_t leaf_mode, const mode_t ancestor_mode = 0700,
	                          const std::optional<uid_t> owner_uid = DefaultSecureOwnerUid(),
	                          const file_security_internal::ValidationRoot &validation_root = {})
	    -> SecurePathCheckResult {
		auto resolved = file_security_internal::ResolveDirectoryRoot(path, label, validation_root);
		if (!resolved.check.ok) {
			return std::move(resolved.check);
		}

		ScopedFd current(
		    open(resolved.root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
		if (!current.Valid()) {
			return {
			    .ok            = false,
			    .error_message = "Failed to open directory root: " + resolved.root.string(),
			    .error_code    = errno,
			};
		}

		struct stat root_st{};
		if (fstat(current.Get(), &root_st) != 0) {
			return {
			    .ok            = false,
			    .error_message = "Failed to stat directory root: " + resolved.root.string(),
			    .error_code    = errno,
			};
		}
		auto root_security = CheckSecurePathStat(root_st, SecurePathKind::kDirectory, resolved.root,
		                                         label, owner_uid);
		if (!root_security.ok) {
			return root_security;
		}

		const auto normalized   = path.lexically_normal();
		auto       current_path = resolved.root;
		for (const auto &comp : resolved.relative) {
			if (comp.empty() || comp == ".") {
				continue;
			}
			current_path /= comp;
			const mode_t mode = (current_path == normalized) ? leaf_mode : ancestor_mode;

			auto [status, next] = file_security_internal::EnterSecureDirectoryComponent(
			    current.Get(), comp, mode, current_path, label, owner_uid, validation_root);
			if (!status.ok) {
				return status;
			}
			current = std::move(next);
		}

		return {
		    .ok = true,
		};
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
