#pragma once

// Readiness contracts, implemented by readiness.cpp. No codec or writer contracts belong here:
// readiness.cpp includes no private store header but this one, and builds into the library root
// PAM links.

#include "storage/user_model_status.hpp"
#include "storage/user_model_store.hpp"
#include "storage/user_model_types.hpp"

#include <filesystem>
#include <string>

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

	[[nodiscard]] auto UserModelInspectionFailedMessage(const std::filesystem::path &path,
	                                                    int error_number) -> std::string;

	[[nodiscard]] auto SnapshotFromStat(const struct stat &st) -> UserModelFileSnapshot;

	// Regular file owned by `owner_uid`, not group- or world-writable, one link, and non-empty.
	// It backs OpenRuntimeDescriptor and, through LoadDocumentFromFd, every path load
	// (ListEntries, LoadModels), the append and remove documents, and LoadUserModelsFromFd.
	// Writers check the file they open with CheckSecurePathStat directly
	// (LockNamespaceAndOpenModelFile), and clear never loads a document, so a new file rule
	// added here must also be added there to cover them. The non-empty rule is the exception:
	// it is for readers only, so clear can still remove a damaged empty model.
	[[nodiscard]] auto ValidateOpenedModelFd(int fd, const std::filesystem::path &path,
	                                         uid_t owner_uid) -> ValidatedModelFd;

	[[nodiscard]] auto CheckReadiness(const UserModelStoreConfig &config, const std::string &user)
	    -> UserModelReadinessResult;

	[[nodiscard]] auto OpenRuntimeDescriptor(const UserModelStoreConfig &config,
	                                         const std::string &user) -> UserModelDescriptorResult;

	// The read-only path walk: CheckReadiness as a path result. Never creates anything.
	[[nodiscard]] auto ResolvePath(const UserModelStoreConfig &config, const std::string &user)
	    -> UserModelPathResult;

}  // namespace howdy::native::user_model_store_internal
