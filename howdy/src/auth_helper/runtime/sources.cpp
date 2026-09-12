#include "auth_helper/runtime/internal.hpp"
#include "internal.hpp"
#include "storage/user_model_readiness.hpp"
#include "storage/user_model_status.hpp"
#include "support/fd_io.hpp"
#include "support/user_names.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>

#include <sys/stat.h>
#include <sys/types.h>

namespace howdy::native::auth_helper {
	namespace {
		constexpr std::size_t kCopyBufferSize = std::size_t{64} * 1024;

		auto LogSourcesErrnoFailure(std::string_view operation, const std::filesystem::path &path,
		                            int error_number) -> bool {
			std::cerr << "Failed to " << operation << " '" << path
			          << "': " << std::strerror(error_number) << "\n";
			return false;
		}

		auto StatUnchanged(const struct stat &before, const struct stat &after) -> bool {
			return before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
			       before.st_size == after.st_size &&
			       before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
			       before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
			       before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
			       before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
		}

		auto SeekStart(int fd) -> bool {
			return lseek(fd, 0, SEEK_SET) == 0;
		}
	}  // namespace

	namespace runtime_internal {

		auto SourceUnchanged(const SourceFile &source) -> bool {
			struct stat current{};
			return fstat(source.fd.Get(), &current) == 0 &&
			       StatUnchanged(source.initial_stat, current);
		}

		auto CompareFiles(int left_fd, int right_fd) -> bool {
			if (!SeekStart(left_fd) || !SeekStart(right_fd)) {
				return false;
			}
			std::array<char, kCopyBufferSize> left{};
			std::array<char, kCopyBufferSize> right{};
			while (true) {
				ssize_t left_size;
				do {
					left_size = read(left_fd, left.data(), left.size());
				} while (left_size < 0 && errno == EINTR);
				if (left_size < 0) {
					return false;
				}
				ssize_t right_size;
				do {
					right_size = read(right_fd, right.data(), right.size());
				} while (right_size < 0 && errno == EINTR);
				if (right_size < 0 || left_size != right_size) {
					return false;
				}
				if (left_size == 0) {
					return true;
				}
				if (std::memcmp(left.data(), right.data(), static_cast<std::size_t>(left_size)) !=
				    0) {
					return false;
				}
			}
		}

		auto CopySourceToOpenFile(const SourceFile &source, int destination_fd) -> bool {
			if (!SeekStart(source.fd.Get()) || ftruncate(destination_fd, 0) != 0 ||
			    !SeekStart(destination_fd)) {
				return false;
			}
			std::array<char, kCopyBufferSize> buffer{};
			while (true) {
				ssize_t size;
				do {
					size = read(source.fd.Get(), buffer.data(), buffer.size());
				} while (size < 0 && errno == EINTR);
				if (size < 0) {
					return false;
				}
				if (size == 0) {
					break;
				}
				if (!howdy::native::WriteAllToFd(destination_fd, buffer.data(),
				                                 static_cast<std::size_t>(size))) {
					return false;
				}
			}
			return howdy::native::SyncFd(destination_fd) && SourceUnchanged(source);
		}

		auto OpenSourceFile(const std::filesystem::path &path, const std::string &label,
		                    uid_t owner_uid) -> std::optional<SourceFile> {
			UniqueFd fd(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
			if (fd.Get() < 0) {
				LogSourcesErrnoFailure("open " + label, path, errno);
				return std::nullopt;
			}
			if (!internal::SecureSourceFileStat(fd.Get(), label, owner_uid)) {
				return std::nullopt;
			}
			SourceFile source{.fd = std::move(fd)};
			if (fstat(source.fd.Get(), &source.initial_stat) != 0) {
				return std::nullopt;
			}
			return source;
		}

	}  // namespace runtime_internal

	namespace internal {

		auto SecureSourceFileStat(int fd, const std::string &label, uid_t owner_uid) -> bool {
			struct stat stat{};
			if (fstat(fd, &stat) != 0) {
				std::cerr << "Failed to inspect " << label << ": " << std::strerror(errno) << "\n";
				return false;
			}
			if (!S_ISREG(stat.st_mode) || stat.st_uid != owner_uid ||
			    (stat.st_mode & (S_IWGRP | S_IWOTH)) != 0 || stat.st_nlink != 1) {
				std::cerr << label << " failed secure file validation\n";
				return false;
			}
			return true;
		}

		auto SelectSourceModelPath(const std::filesystem::path &source_user_models_dir,
		                           const std::string &user, std::optional<uid_t> owner_uid,
		                           std::optional<std::filesystem::path>         &source_model_path,
		                           const file_security_internal::ValidationRoot &validation_root)
		    -> bool {
			source_model_path.reset();
			const auto readiness = howdy::native::CheckUserModelReadiness(
			    source_user_models_dir, user, owner_uid, validation_root);
			switch (readiness.status) {
				case howdy::native::UserModelStatus::kOk:
					source_model_path = readiness.path;
					return true;
				case howdy::native::UserModelStatus::kNoModel:
				case howdy::native::UserModelStatus::kNoModelDirectory:
					return true;
				case howdy::native::UserModelStatus::kInvalidUser:
					std::cerr << howdy::native::kInvalidUserNameMessage << "\n";
					return false;
				default:
					std::cerr << (readiness.error_message.empty()
					                  ? "Failed to validate user model file"
					                  : readiness.error_message)
					          << "\n";
					return false;
			}
		}

	}  // namespace internal
}  // namespace howdy::native::auth_helper
