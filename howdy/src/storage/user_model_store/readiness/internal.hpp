#pragma once

// Readiness contracts, implemented by readiness.cpp. No codec or writer contracts belong here:
// readiness.cpp includes no private store header but this one, and builds into the library root
// PAM links.

#include "storage/user_model_status.hpp"
#include "storage/user_model_store.hpp"
#include "storage/user_model_types.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include <sys/stat.h>
#include <sys/types.h>

namespace howdy::native::user_model_store_internal {

	// Fail closed: a result that names no status is a failure, never a missing model.
	struct UserModelReadinessResult {
		UserModelStatus       status = UserModelStatus::kParseError;
		std::string           error_message;
		std::filesystem::path path;
	};

	// Fail closed: a result that names no status is a failure, never a resolved path.
	struct UserModelPathResult {
		UserModelStatus       status = UserModelStatus::kParseError;
		std::string           error_message;
		std::filesystem::path path;
	};

	// Fail closed: a result that names no status never passes as a validated descriptor.
	struct ValidatedModelFd {
		UserModelStatus status = UserModelStatus::kParseError;
		std::string     error_message;
		struct stat     file_stat{};
	};

	[[nodiscard]] auto WithErrno(std::string message, int error_number) -> std::string;

	[[nodiscard]] auto OpenErrorMeansNotRegularFile(int error_number) -> bool;

	[[nodiscard]] auto NotRegularFileMessage(std::string_view             label,
	                                         const std::filesystem::path &path) -> std::string;

	[[nodiscard]] auto UserModelInspectionFailedMessage(const std::filesystem::path &path,
	                                                    int error_number) -> std::string;

	[[nodiscard]] auto SnapshotFromStat(const struct stat &st) -> UserModelFileSnapshot;

	enum class EmptyModelPolicy : std::uint8_t {
		kReject,
		// For writers and inspection, so clear can still remove a damaged empty model.
		kAccept,
	};

	// The one check of an opened model file, for every reader, writer and inspection: a regular
	// file owned by `owner_uid`, not group- or world-writable, with one link, and not empty unless
	// `empty_policy` accepts that.
	[[nodiscard]] auto ValidateOpenedModelFd(int fd, const std::filesystem::path &path,
	                                         uid_t owner_uid, EmptyModelPolicy empty_policy)
	    -> ValidatedModelFd;

	[[nodiscard]] auto CheckReadiness(const UserModelStoreConfig &config, const std::string &user)
	    -> UserModelReadinessResult;

	[[nodiscard]] auto OpenRuntimeDescriptor(const UserModelStoreConfig &config,
	                                         const std::string &user) -> UserModelDescriptorResult;

	// The read-only path walk: CheckReadiness as a path result. Never creates anything.
	[[nodiscard]] auto ResolvePath(const UserModelStoreConfig &config, const std::string &user)
	    -> UserModelPathResult;

}  // namespace howdy::native::user_model_store_internal
