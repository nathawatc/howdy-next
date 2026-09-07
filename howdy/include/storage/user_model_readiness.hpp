#pragma once

#include "storage/user_model_status.hpp"

#include <filesystem>
#include <optional>
#include <string>

#include <sys/types.h>

namespace howdy::native {

	struct UserModelReadinessResult {
		UserModelStatus       status = UserModelStatus::kNoModel;
		std::string           error_message;
		std::filesystem::path path;
	};

	auto ValidateStagedUserModelFile(int fd, const std::filesystem::path &path,
	                                 std::optional<uid_t> owner_uid = static_cast<uid_t>(0))
	    -> bool;

	auto CheckUserModelReadiness(const std::filesystem::path &models_dir, const std::string &user,
	                             std::optional<uid_t> owner_uid) -> UserModelReadinessResult;

}  // namespace howdy::native
