#include "storage/user_model_readiness.hpp"

#include "storage/user_model_status.hpp"
#include "support/file_security.hpp"
#include "support/user_names.hpp"
#include "user_model_readiness/internal.hpp"

#include <cerrno>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

namespace howdy::native {

	namespace {

		using user_model_readiness_internal::ClassifyStagedPath;
		using user_model_readiness_internal::InspectStagedModel;
		using user_model_readiness_internal::StagedPathKind;
		using user_model_readiness_internal::StagedReadiness;
		using user_model_readiness_internal::ValidateStagedModelFile;

		auto ReadinessFailure(UserModelStatus status, std::string message,
		                      std::filesystem::path path = {}) -> UserModelReadinessResult {
			return UserModelReadinessResult{
			    .status        = status,
			    .error_message = std::move(message),
			    .path          = std::move(path),
			};
		}

	}  // namespace

	auto ValidateStagedUserModelFile(int fd, const std::filesystem::path &path,
	                                 std::optional<uid_t> owner_uid) -> bool {
		return ValidateStagedModelFile(fd, path, owner_uid);
	}

	auto CheckUserModelReadiness(const std::filesystem::path &models_dir, const std::string &user,
	                             std::optional<uid_t>                          owner_uid,
	                             const file_security_internal::ValidationRoot &validation_root)
	    -> UserModelReadinessResult {
		const auto model_path = ResolveUserModelPath(models_dir, user);
		if (!model_path) {
			return ReadinessFailure(UserModelStatus::kInvalidUser, kInvalidUserNameMessage);
		}

		const auto staged_kind = ClassifyStagedPath(*model_path);
		if (staged_kind != StagedPathKind::kCanonical) {
			if (staged_kind == StagedPathKind::kMalformed) {
				return ReadinessFailure(UserModelStatus::kInsecurePath,
				                        "Staged user model path failed validation: " +
				                            model_path->string(),
				                        *model_path);
			}
			switch (InspectStagedModel(*model_path, owner_uid)) {
				case StagedReadiness::kPresent:
					return UserModelReadinessResult{.status = UserModelStatus::kOk,
					                                .path   = *model_path};
				case StagedReadiness::kAbsent:
					return ReadinessFailure(UserModelStatus::kNoModel, {}, *model_path);
				case StagedReadiness::kError:
					return ReadinessFailure(UserModelStatus::kParseError,
					                        "Failed to inspect staged user model: " +
					                            model_path->string(),
					                        *model_path);
				case StagedReadiness::kInsecure:
					return ReadinessFailure(UserModelStatus::kInsecurePath,
					                        "Staged user model failed security validation: " +
					                            model_path->string(),
					                        *model_path);
			}
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
