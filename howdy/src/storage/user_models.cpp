#include "storage/user_models.hpp"

#include "storage/user_model_status.hpp"
#include "storage/user_model_types.hpp"
#include "support/file_security.hpp"
#include "user_model_store/internal.hpp"

#include <filesystem>
#include <string>

#include <sys/types.h>

namespace howdy::native {

	auto LoadUserModelsFromFd(int model_fd, const std::filesystem::path &display_path,
	                          const std::string &expected_backend,
	                          const std::string &expected_model, uid_t owner_uid)
	    -> UserModelLoadResult {
		const auto read_only = CheckReadOnlyFd(model_fd, display_path, kUserModelFileLabel);
		if (!read_only.ok) {
			return UserModelLoadResult{
			    .status        = UserModelStatus::kInsecurePath,
			    .error_message = read_only.error_message,
			};
		}
		auto document = user_model_store_internal::LoadDocumentFromFd(
		    model_fd, display_path,
		    {.backend = expected_backend, .model = expected_model, .strict_shape = false},
		    owner_uid);
		return user_model_store_internal::FlattenLoadedModels(document);
	}

}  // namespace howdy::native
