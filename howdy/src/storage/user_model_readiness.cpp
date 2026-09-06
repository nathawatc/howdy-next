#include "storage/user_model_readiness.hpp"

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

		using user_model_readiness_internal::classify_staged_path;
		using user_model_readiness_internal::inspect_staged_model;
		using user_model_readiness_internal::StagedPathKind;
		using user_model_readiness_internal::StagedReadiness;
		using user_model_readiness_internal::validate_staged_model_file;

		auto readiness_failure(UserModelStatus status, std::string message,
		                       std::filesystem::path path = {}) -> UserModelReadinessResult {
			return UserModelReadinessResult{
			    .status        = status,
			    .error_message = std::move(message),
			    .path          = std::move(path),
			};
		}

	}  // namespace

	auto validate_staged_user_model_file(int fd, const std::filesystem::path &path,
	                                     std::optional<uid_t> owner_uid) -> bool {
		return validate_staged_model_file(fd, path, owner_uid);
	}

	auto check_user_model_readiness(const std::filesystem::path &models_dir,
	                                const std::string &user, std::optional<uid_t> owner_uid)
	    -> UserModelReadinessResult {
		const auto model_path = resolve_user_model_path(models_dir, user);
		if (!model_path) {
			return readiness_failure(UserModelStatus::kInvalidUser, kInvalidUserNameMessage);
		}

		const auto staged_kind = classify_staged_path(*model_path);
		if (staged_kind != StagedPathKind::kCanonical) {
			if (staged_kind == StagedPathKind::kMalformed) {
				return readiness_failure(UserModelStatus::kInsecurePath,
				                         "Staged user model path failed validation: " +
				                             model_path->string(),
				                         *model_path);
			}
			switch (inspect_staged_model(*model_path, owner_uid)) {
				case StagedReadiness::kPresent:
					return UserModelReadinessResult{.status = UserModelStatus::kOk,
					                                .path   = *model_path};
				case StagedReadiness::kAbsent:
					return readiness_failure(UserModelStatus::kNoModel, {}, *model_path);
				case StagedReadiness::kError:
					return readiness_failure(UserModelStatus::kParseError,
					                         "Failed to inspect staged user model: " +
					                             model_path->string(),
					                         *model_path);
				case StagedReadiness::kInsecure:
					return readiness_failure(UserModelStatus::kInsecurePath,
					                         "Staged user model failed security validation: " +
					                             model_path->string(),
					                         *model_path);
			}
		}

		const auto directory_security = check_secure_root_owned_directory_tree(
		    models_dir, kUserModelsDirectoryLabel, owner_uid);
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
		    *model_path, {.directory = kUserModelsDirectoryLabel, .file = kUserModelFileLabel},
		    owner_uid);
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
