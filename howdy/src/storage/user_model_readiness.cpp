#include "storage/user_model_readiness.hpp"

#include "storage/user_model_path.hpp"
#include "storage/user_model_status.hpp"
#include "support/file_security.hpp"
#include "support/file_security/validation_root.hpp"
#include "support/user_names.hpp"

#include <cerrno>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

#include <sys/types.h>

namespace howdy::native {

	namespace {

		auto ReadinessFailure(UserModelStatus status, std::string message,
		                      std::filesystem::path path = {}) -> UserModelReadinessResult {
			return UserModelReadinessResult{
			    .status        = status,
			    .error_message = std::move(message),
			    .path          = std::move(path),
			};
		}

	}  // namespace

	auto CheckUserModelReadiness(const std::filesystem::path &models_dir, const std::string &user,
	                             std::optional<uid_t>                          owner_uid,
	                             const file_security_internal::ValidationRoot &validation_root)
	    -> UserModelReadinessResult {
		const auto model_path = ResolveUserModelPath(models_dir, user);
		if (!model_path) {
			return ReadinessFailure(UserModelStatus::kInvalidUser, kInvalidUserNameMessage);
		}

		const auto directory_security = CheckSecureRootOwnedDirectoryTree(
		    models_dir, kUserModelsDirectoryLabel, owner_uid, validation_root);
		if (!directory_security.ok) {
			if (directory_security.error_code == ENOENT) {
				return ReadinessFailure(UserModelStatus::kNoModelDirectory, {}, *model_path);
			}
			if (directory_security.error_code != 0) {
				return ReadinessFailure(UserModelStatus::kParseError,
				                        directory_security.error_message, *model_path);
			}
			return ReadinessFailure(UserModelStatus::kInsecurePath,
			                        directory_security.error_message, *model_path);
		}

		const auto file_security = CheckSecureRootOwnedFileWithDirectory(
		    *model_path, {.directory = kUserModelsDirectoryLabel, .file = kUserModelFileLabel},
		    owner_uid, validation_root);
		if (!file_security.ok) {
			if (file_security.error_code == ENOENT) {
				return ReadinessFailure(UserModelStatus::kNoModel, {}, *model_path);
			}
			if (file_security.error_code != 0) {
				return ReadinessFailure(UserModelStatus::kParseError, file_security.error_message,
				                        *model_path);
			}
			return ReadinessFailure(UserModelStatus::kInsecurePath, file_security.error_message,
			                        *model_path);
		}

		return UserModelReadinessResult{
		    .status = UserModelStatus::kOk,
		    .path   = *model_path,
		};
	}

}  // namespace howdy::native
