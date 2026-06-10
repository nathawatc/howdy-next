#include "storage/user_model_readiness.hpp"

#include "common/file_security.hpp"
#include "common/user_names.hpp"

#include <cerrno>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

namespace howdy::native {

	namespace {

		auto readiness_failure(UserModelStatus status, std::string message,
		                       std::filesystem::path path = {}) -> UserModelReadinessResult {
			return UserModelReadinessResult{
			    .status        = status,
			    .error_message = std::move(message),
			    .path          = std::move(path),
			};
		}

	}  // namespace

	auto check_user_model_readiness(const std::filesystem::path &models_dir,
	                                const std::string &user, std::optional<uid_t> owner_uid)
	    -> UserModelReadinessResult {
		const auto model_path = resolve_user_model_path(models_dir, user);
		if (!model_path) {
			return readiness_failure(UserModelStatus::kInvalidUser, kInvalidUserNameMessage);
		}

		const auto directory_security =
		    check_secure_root_owned_directory_tree(models_dir, "User models directory", owner_uid);
		if (!directory_security.ok) {
			if (directory_security.error_code == ENOENT) {
				return readiness_failure(UserModelStatus::kNoModelDirectory, {}, *model_path);
			}
			if (directory_security.error_code != 0) {
				return readiness_failure(UserModelStatus::kParseError,
				                         directory_security.error_message, *model_path);
			}
			return readiness_failure(UserModelStatus::kInsecurePath,
			                         directory_security.error_message, *model_path);
		}

		const auto file_security = check_secure_root_owned_file_with_directory(
		    *model_path, "User models directory", "User model file", owner_uid);
		if (!file_security.ok) {
			if (file_security.error_code == ENOENT) {
				return readiness_failure(UserModelStatus::kNoModel, {}, *model_path);
			}
			if (file_security.error_code != 0) {
				return readiness_failure(UserModelStatus::kParseError, file_security.error_message,
				                         *model_path);
			}
			return readiness_failure(UserModelStatus::kInsecurePath, file_security.error_message,
			                         *model_path);
		}

		return UserModelReadinessResult{
		    .status = UserModelStatus::kOk,
		    .path   = *model_path,
		};
	}

}  // namespace howdy::native
