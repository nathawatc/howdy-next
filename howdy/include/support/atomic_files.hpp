#pragma once

#include "support/fd_io.hpp"
#include "support/scoped_fd.hpp"

#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <sys/syscall.h>

#include <linux/fs.h>

namespace howdy::native {

	inline constexpr mode_t kDefaultAtomicFileMode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;

	struct StagedFile {
		ScopedFd              fd;
		std::filesystem::path path;
	};

	enum class StagedFileMetadataPolicy : std::uint8_t {
		kPreserveExisting,
		kUseDefaultMode,
	};

	enum class StagedFileParentPolicy : std::uint8_t {
		kCreate,
		kRequireExisting,
	};

	enum class AtomicFileInstallPolicy : std::uint8_t {
		kReplaceExisting,
		kNoReplaceExisting,
	};

	enum class AtomicFileCommitResult : std::uint8_t {
		kNotCommitted,
		kAtomicExchangeUnsupported,
		kDestinationExists,
		kCommitted,
		kCommittedSyncFailed,
		kStateUncertain,
	};

	[[nodiscard]] constexpr auto AtomicFileMayHaveCommitted(AtomicFileCommitResult result) -> bool {
		return result == AtomicFileCommitResult::kCommitted ||
		       result == AtomicFileCommitResult::kCommittedSyncFailed ||
		       result == AtomicFileCommitResult::kStateUncertain;
	}

	[[nodiscard]] constexpr auto AtomicFileCommitIsDurable(AtomicFileCommitResult result) -> bool {
		return result == AtomicFileCommitResult::kCommitted;
	}

	using SyncParentDirectoryFn = bool (*)(const std::filesystem::path &path);

	inline auto SyncParentDirectory(const std::filesystem::path &path) -> bool {
		const auto parent =
		    path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
		ScopedFd dir_fd(open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
		if (dir_fd.Get() < 0) {
			return false;
		}

		if (!SyncFd(dir_fd.Get())) {
			return false;
		}
		return dir_fd.Close();
	}

	inline auto RemoveFileAndSync(const std::filesystem::path &path) -> AtomicFileCommitResult {
		std::error_code ec;
		const bool      removed = std::filesystem::remove(path, ec);
		if (ec || !removed) {
			return AtomicFileCommitResult::kNotCommitted;
		}
		return SyncParentDirectory(path) ? AtomicFileCommitResult::kCommitted
		                                 : AtomicFileCommitResult::kCommittedSyncFailed;
	}

	inline void CleanupStagedFile(StagedFile &staged) {
		staged.fd.Reset();
		std::error_code ec;
		std::filesystem::remove(staged.path, ec);
	}

	inline auto PrepareStagedFile(
	    const std::filesystem::path &destination, std::string_view temp_prefix,
	    mode_t                   default_mode    = kDefaultAtomicFileMode,
	    StagedFileMetadataPolicy metadata_policy = StagedFileMetadataPolicy::kPreserveExisting,
	    StagedFileParentPolicy   parent_policy   = StagedFileParentPolicy::kCreate)
	    -> std::optional<StagedFile> {
		const auto parent = destination.parent_path().empty() ? std::filesystem::path(".")
		                                                      : destination.parent_path();
		if (parent_policy == StagedFileParentPolicy::kRequireExisting) {
			ScopedFd parent_fd(
			    open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
			if (parent_fd.Get() < 0) {
				return std::nullopt;
			}
		} else {
			std::error_code create_ec;
			std::filesystem::create_directories(parent, create_ec);
			if (create_ec) {
				return std::nullopt;
			}
		}

		struct stat current_stat{};
		const bool  have_current_stat = lstat(destination.c_str(), &current_stat) == 0;
		if (have_current_stat && !S_ISREG(current_stat.st_mode)) {
			return std::nullopt;
		}

		std::string       temp_template = (parent / (std::string(temp_prefix) + "XXXXXX")).string();
		std::vector<char> writable(temp_template.begin(), temp_template.end());
		writable.push_back('\0');

		ScopedFd fd(mkostemp(writable.data(), O_CLOEXEC));
		if (fd.Get() < 0) {
			return std::nullopt;
		}

		const std::filesystem::path temp_path(writable.data());
		if (have_current_stat && metadata_policy == StagedFileMetadataPolicy::kPreserveExisting) {
			if (fchown(fd.Get(), current_stat.st_uid, current_stat.st_gid) != 0 ||
			    fchmod(fd.Get(), current_stat.st_mode & 07777) != 0) {
				fd.Reset();
				std::error_code ec;
				std::filesystem::remove(temp_path, ec);
				return std::nullopt;
			}
		} else if (fchmod(fd.Get(), default_mode) != 0) {
			fd.Reset();
			std::error_code ec;
			std::filesystem::remove(temp_path, ec);
			return std::nullopt;
		}

		return StagedFile{.fd = std::move(fd), .path = temp_path};
	}

	inline auto InstallStagedFile(
	    StagedFile &staged, const std::filesystem::path &destination,
	    SyncParentDirectoryFn   sync_parent    = SyncParentDirectory,
	    AtomicFileInstallPolicy install_policy = AtomicFileInstallPolicy::kReplaceExisting)
	    -> AtomicFileCommitResult {
		if (staged.path.empty()) {
			staged.fd.Reset();
			return AtomicFileCommitResult::kNotCommitted;
		}
		if (staged.fd.Get() < 0) {
			CleanupStagedFile(staged);
			return AtomicFileCommitResult::kNotCommitted;
		}

		if (!SyncFd(staged.fd.Get())) {
			CleanupStagedFile(staged);
			return AtomicFileCommitResult::kNotCommitted;
		}

		if (!staged.fd.Close()) {
			CleanupStagedFile(staged);
			return AtomicFileCommitResult::kNotCommitted;
		}

		if (install_policy == AtomicFileInstallPolicy::kReplaceExisting) {
			std::error_code ec;
			std::filesystem::rename(staged.path, destination, ec);
			if (ec) {
				CleanupStagedFile(staged);
				return AtomicFileCommitResult::kNotCommitted;
			}
		} else {
#if defined(SYS_renameat2) && defined(RENAME_NOREPLACE)
			long rename_result = -1;
			do {
				rename_result = syscall(SYS_renameat2, AT_FDCWD, staged.path.c_str(), AT_FDCWD,
				                        destination.c_str(), RENAME_NOREPLACE);
			} while (rename_result != 0 && errno == EINTR);
			if (rename_result != 0) {
				const int error_number = errno;
				CleanupStagedFile(staged);
				return error_number == EEXIST ? AtomicFileCommitResult::kDestinationExists
				                              : AtomicFileCommitResult::kNotCommitted;
			}
#else
			cleanup_staged_file(staged);
			return AtomicFileCommitResult::kNotCommitted;
#endif
		}
		const bool parent_synced = sync_parent != nullptr && sync_parent(destination);
		staged.path.clear();
		return parent_synced ? AtomicFileCommitResult::kCommitted
		                     : AtomicFileCommitResult::kCommittedSyncFailed;
	}

	inline auto WriteAtomicFile(const std::filesystem::path &path, std::string_view content,
	                            mode_t default_mode = kDefaultAtomicFileMode)
	    -> AtomicFileCommitResult {
		auto staged = PrepareStagedFile(path, ".howdy-atomic-", default_mode);
		if (!staged.has_value()) {
			return AtomicFileCommitResult::kNotCommitted;
		}

		if (!WriteAllToFd(staged->fd.Get(), content)) {
			CleanupStagedFile(*staged);
			return AtomicFileCommitResult::kNotCommitted;
		}

		return InstallStagedFile(*staged, path);
	}

}  // namespace howdy::native
