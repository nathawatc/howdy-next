#pragma once

#include "support/bit_flags.hpp"
#include "support/eintr.hpp"
#include "support/fd_io.hpp"
#include "support/scoped_fd.hpp"

#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>

#include <linux/fs.h>

namespace howdy::native {

	inline constexpr mode_t kDefaultAtomicFileMode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;

	class StagedFile {
	public:
		StagedFile() noexcept = default;

		StagedFile(ScopedFd staged_fd, std::filesystem::path staged_path) noexcept
		    : fd_(std::move(staged_fd))
		    , path_(std::move(staged_path)) {}

		StagedFile(const StagedFile &)                     = delete;
		auto operator=(const StagedFile &) -> StagedFile & = delete;

		StagedFile(StagedFile &&other) noexcept
		    : fd_(std::move(other.fd_))
		    , path_(std::move(other.path_)) {
			other.path_.clear();
		}

		auto operator=(StagedFile &&other) noexcept -> StagedFile & {
			if (this != &other) {
				Cleanup();
				fd_   = std::move(other.fd_);
				path_ = std::move(other.path_);
				other.path_.clear();
			}
			return *this;
		}

		// Only unlinks an uncommitted staging path; commit clears `path_` immediately after rename.
		~StagedFile() {
			Cleanup();
		}

		[[nodiscard]] auto Fd() noexcept -> ScopedFd & {
			return fd_;
		}

		[[nodiscard]] auto Fd() const noexcept -> const ScopedFd & {
			return fd_;
		}

		[[nodiscard]] auto Path() const noexcept -> const std::filesystem::path & {
			return path_;
		}

		void Disarm() noexcept {
			path_.clear();
		}

		void Cleanup() noexcept {
			fd_.Reset();
			if (!path_.empty()) {
				std::error_code ec;
				std::filesystem::remove(path_, ec);
				path_.clear();
			}
		}

	private:
		ScopedFd              fd_;
		std::filesystem::path path_;
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
		// The kernel or filesystem lacks the renameat2 operation the commit needs.
		kAtomicRenameUnsupported,
		// A no-replace install found its destination taken.
		kDestinationExists,
		// The destination no longer names what the caller opened.
		kDestinationChanged,
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

	enum class AtomicRenameMode : std::uint8_t {
		// Swap two existing paths (RENAME_EXCHANGE).
		kExchange,
		// Link a new path in only if nothing is there yet (RENAME_NOREPLACE).
		kNoReplace,
	};

	enum class AtomicRenameResult : std::uint8_t {
		kSuccess,
		kFailed,
		kUnsupported,
		kDestinationExists,
	};

	inline auto AtomicRenameResultForErrno(int error_number) -> AtomicRenameResult {
		if (error_number == EEXIST) {
			return AtomicRenameResult::kDestinationExists;
		}
		if (error_number == ENOSYS || error_number == EINVAL || error_number == EOPNOTSUPP) {
			return AtomicRenameResult::kUnsupported;
		}
#ifdef ENOTSUP
		if (error_number == ENOTSUP) {
			return AtomicRenameResult::kUnsupported;
		}
#endif
		return AtomicRenameResult::kFailed;
	}

	// renameat2 in `mode`, retried on EINTR, in rename(2)'s argument order. kUnsupported when
	// the build, the kernel or the filesystem lacks the operation.
	// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
	inline auto AtomicRename(const std::filesystem::path &from, const std::filesystem::path &to,
	                         AtomicRenameMode mode) -> AtomicRenameResult {
#if defined(SYS_renameat2) && defined(RENAME_EXCHANGE) && defined(RENAME_NOREPLACE)
		const unsigned int flags =
		    mode == AtomicRenameMode::kExchange ? RENAME_EXCHANGE : RENAME_NOREPLACE;
		if (RetryOnEintr([&] -> decltype(syscall(0)) {
			    return syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), flags);
		    }) != 0) {
			return AtomicRenameResultForErrno(errno);
		}
		return AtomicRenameResult::kSuccess;
#else
		(void)from;
		(void)to;
		(void)mode;
		errno = ENOSYS;
		return AtomicRenameResult::kUnsupported;
#endif
	}

	using AtomicRenameFn = AtomicRenameResult (*)(const std::filesystem::path &from,
	                                              const std::filesystem::path &to,
	                                              AtomicRenameMode             mode);

	// The commit result of a rename that did not succeed.
	[[nodiscard]] constexpr auto FailedAtomicRenameCommitResult(AtomicRenameResult result)
	    -> AtomicFileCommitResult {
		if (result == AtomicRenameResult::kUnsupported) {
			return AtomicFileCommitResult::kAtomicRenameUnsupported;
		}
		if (result == AtomicRenameResult::kDestinationExists) {
			return AtomicFileCommitResult::kDestinationExists;
		}
		return AtomicFileCommitResult::kNotCommitted;
	}

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

	inline void CleanupStagedFile(StagedFile &staged) noexcept {
		staged.Cleanup();
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
			ScopedFd const parent_fd(
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

		StagedFile staged(std::move(fd), writable.data());
		if (have_current_stat && metadata_policy == StagedFileMetadataPolicy::kPreserveExisting) {
			if (fchown(staged.Fd().Get(), current_stat.st_uid, current_stat.st_gid) != 0 ||
			    fchmod(staged.Fd().Get(), MaskedBitsAs<mode_t>(current_stat.st_mode, 07777)) != 0) {
				return std::nullopt;
			}
		} else if (fchmod(staged.Fd().Get(), default_mode) != 0) {
			return std::nullopt;
		}

		return staged;
	}

	// `sync_parent` and `atomic_rename` may be function pointers or callables that carry their own
	// context, such as a store's test hooks. A null or empty `sync_parent` (nullptr, a null
	// function pointer, or an empty std::function) skips the sync and reports
	// kCommittedSyncFailed. Both are taken by value so a plain function name decays to a pointer.
	template <typename SyncParent = SyncParentDirectoryFn, typename Rename = AtomicRenameFn>
	inline auto InstallStagedFile(
	    StagedFile &staged, const std::filesystem::path &destination,
	    // NOLINTNEXTLINE(performance-unnecessary-value-param): by value so functions decay.
	    SyncParent              sync_parent    = SyncParentDirectory,
	    AtomicFileInstallPolicy install_policy = AtomicFileInstallPolicy::kReplaceExisting,
	    Rename                  atomic_rename  = AtomicRename) -> AtomicFileCommitResult {
		if (staged.Path().empty()) {
			staged.Fd().Reset();
			return AtomicFileCommitResult::kNotCommitted;
		}
		if (staged.Fd().Get() < 0) {
			CleanupStagedFile(staged);
			return AtomicFileCommitResult::kNotCommitted;
		}

		if (!SyncFd(staged.Fd().Get())) {
			CleanupStagedFile(staged);
			return AtomicFileCommitResult::kNotCommitted;
		}

		if (!staged.Fd().Close()) {
			CleanupStagedFile(staged);
			return AtomicFileCommitResult::kNotCommitted;
		}

		if (install_policy == AtomicFileInstallPolicy::kReplaceExisting) {
			std::error_code ec;
			std::filesystem::rename(staged.Path(), destination, ec);
			if (ec) {
				CleanupStagedFile(staged);
				return AtomicFileCommitResult::kNotCommitted;
			}
		} else {
			const auto rename_result =
			    atomic_rename(staged.Path(), destination, AtomicRenameMode::kNoReplace);
			if (rename_result != AtomicRenameResult::kSuccess) {
				CleanupStagedFile(staged);
				return FailedAtomicRenameCommitResult(rename_result);
			}
		}
		staged.Disarm();
		bool parent_synced = false;
		if constexpr (std::is_null_pointer_v<SyncParent>) {
			parent_synced = false;
		} else if constexpr (std::is_constructible_v<bool, const SyncParent &>) {
			parent_synced = static_cast<bool>(sync_parent) && sync_parent(destination);
		} else {
			parent_synced = sync_parent(destination);
		}
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

		if (!WriteAllToFd(staged->Fd().Get(), content)) {
			CleanupStagedFile(*staged);
			return AtomicFileCommitResult::kNotCommitted;
		}

		return InstallStagedFile(*staged, path);
	}

}  // namespace howdy::native
