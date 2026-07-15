#pragma once

#include "common/fd_io.hpp"

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

namespace howdy::native {

	inline constexpr mode_t kDefaultAtomicFileMode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;

	class ScopedFd {
	public:
		ScopedFd() = default;

		explicit ScopedFd(int fd)
		    : fd_(fd) {}

		ScopedFd(const ScopedFd &)                     = delete;
		auto operator=(const ScopedFd &) -> ScopedFd & = delete;

		ScopedFd(ScopedFd &&other) noexcept
		    : fd_(other.release()) {}

		auto operator=(ScopedFd &&other) noexcept -> ScopedFd & {
			if (this != &other) {
				reset(other.release());
			}
			return *this;
		}

		~ScopedFd() {
			reset();
		}

		[[nodiscard]] auto get() const -> int {
			return fd_;
		}

		[[nodiscard]] auto close() -> bool {
			if (fd_ < 0) {
				return true;
			}
			const int fd = release();
			return ::close(fd) == 0;
		}

		void reset(int fd = -1) {
			if (fd_ >= 0) {
				::close(fd_);
			}
			fd_ = fd;
		}

		[[nodiscard]] auto release() -> int {
			const int fd = fd_;
			fd_          = -1;
			return fd;
		}

	private:
		int fd_ = -1;
	};

	struct StagedFile {
		ScopedFd              fd;
		std::filesystem::path path;
	};

	enum class StagedFileMetadataPolicy : std::uint8_t {
		kPreserveExisting,
		kUseDefaultMode,
	};

	enum class AtomicFileCommitResult : std::uint8_t {
		kNotCommitted,
		kCommitted,
		kCommittedSyncFailed,
		kStateUncertain,
	};

	[[nodiscard]] constexpr auto atomic_file_may_have_committed(AtomicFileCommitResult result)
	    -> bool {
		return result != AtomicFileCommitResult::kNotCommitted;
	}

	[[nodiscard]] constexpr auto atomic_file_commit_is_durable(AtomicFileCommitResult result)
	    -> bool {
		return result == AtomicFileCommitResult::kCommitted;
	}

	using SyncParentDirectoryFn = bool (*)(const std::filesystem::path &path);

	inline auto sync_parent_directory(const std::filesystem::path &path) -> bool {
		const auto parent =
		    path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
		ScopedFd dir_fd(open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
		if (dir_fd.get() < 0) {
			return false;
		}

		if (!sync_fd(dir_fd.get())) {
			return false;
		}
		return dir_fd.close();
	}

	inline auto remove_file_and_sync(const std::filesystem::path &path) -> AtomicFileCommitResult {
		std::error_code ec;
		const bool      removed = std::filesystem::remove(path, ec);
		if (ec || !removed) {
			return AtomicFileCommitResult::kNotCommitted;
		}
		return sync_parent_directory(path) ? AtomicFileCommitResult::kCommitted
		                                   : AtomicFileCommitResult::kCommittedSyncFailed;
	}

	inline void cleanup_staged_file(StagedFile &staged) {
		staged.fd.reset();
		std::error_code ec;
		std::filesystem::remove(staged.path, ec);
	}

	inline auto prepare_staged_file(
	    const std::filesystem::path &destination, std::string_view temp_prefix,
	    mode_t                   default_mode    = kDefaultAtomicFileMode,
	    StagedFileMetadataPolicy metadata_policy = StagedFileMetadataPolicy::kPreserveExisting)
	    -> std::optional<StagedFile> {
		const auto      parent = destination.parent_path().empty() ? std::filesystem::path(".")
		                                                           : destination.parent_path();
		std::error_code create_ec;
		std::filesystem::create_directories(parent, create_ec);
		if (create_ec) {
			return std::nullopt;
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
		if (fd.get() < 0) {
			return std::nullopt;
		}

		const std::filesystem::path temp_path(writable.data());
		if (have_current_stat && metadata_policy == StagedFileMetadataPolicy::kPreserveExisting) {
			if (fchown(fd.get(), current_stat.st_uid, current_stat.st_gid) != 0 ||
			    fchmod(fd.get(), current_stat.st_mode & 07777) != 0) {
				fd.reset();
				std::error_code ec;
				std::filesystem::remove(temp_path, ec);
				return std::nullopt;
			}
		} else if (fchmod(fd.get(), default_mode) != 0) {
			fd.reset();
			std::error_code ec;
			std::filesystem::remove(temp_path, ec);
			return std::nullopt;
		}

		return StagedFile{.fd = std::move(fd), .path = temp_path};
	}

	inline auto install_staged_file(StagedFile &staged, const std::filesystem::path &destination,
	                                SyncParentDirectoryFn sync_parent = sync_parent_directory)
	    -> AtomicFileCommitResult {
		if (staged.path.empty()) {
			staged.fd.reset();
			return AtomicFileCommitResult::kNotCommitted;
		}
		if (staged.fd.get() < 0) {
			cleanup_staged_file(staged);
			return AtomicFileCommitResult::kNotCommitted;
		}

		if (!sync_fd(staged.fd.get())) {
			cleanup_staged_file(staged);
			return AtomicFileCommitResult::kNotCommitted;
		}

		if (!staged.fd.close()) {
			cleanup_staged_file(staged);
			return AtomicFileCommitResult::kNotCommitted;
		}

		std::error_code ec;
		std::filesystem::rename(staged.path, destination, ec);
		if (ec) {
			cleanup_staged_file(staged);
			return AtomicFileCommitResult::kNotCommitted;
		}
		const bool parent_synced = sync_parent != nullptr && sync_parent(destination);
		staged.path.clear();
		return parent_synced ? AtomicFileCommitResult::kCommitted
		                     : AtomicFileCommitResult::kCommittedSyncFailed;
	}

	inline auto write_atomic_file(const std::filesystem::path &path, std::string_view content,
	                              mode_t default_mode = kDefaultAtomicFileMode)
	    -> AtomicFileCommitResult {
		auto staged = prepare_staged_file(path, ".howdy-atomic-", default_mode);
		if (!staged.has_value()) {
			return AtomicFileCommitResult::kNotCommitted;
		}

		if (!write_all_to_fd(staged->fd.get(), content)) {
			cleanup_staged_file(*staged);
			return AtomicFileCommitResult::kNotCommitted;
		}

		return install_staged_file(*staged, path);
	}

}  // namespace howdy::native
