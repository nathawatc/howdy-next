#include "auth_helper/runtime_descriptors.hpp"

#include "config/config_utils.hpp"
#include "protocol/runtime_descriptor_message.hpp"
#include "storage/user_model_readiness.hpp"
#include "storage/user_model_status.hpp"
#include "support/error_message.hpp"
#include "support/file_security.hpp"
#include "support/scoped_fd.hpp"
#include "support/user_names.hpp"

#include <cerrno>
#include <string>
#include <utility>

#include <fcntl.h>

#include <sys/stat.h>

namespace howdy::native::auth_helper {

	namespace {

		using runtime_descriptor_message::ModelState;

		constexpr int kOpenFlags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;

		auto OpenConfig(const RuntimeDescriptorSources &sources, OpenedRuntimeDescriptors *opened)
		    -> bool {
			howdy::native::ScopedFd config(open(sources.config.c_str(), kOpenFlags));
			if (!config.Valid()) {
				const int error_number = errno;
				opened->error_message  = "Failed to open Config file: " + sources.config.string() +
				                         " (" + SystemErrorMessage(error_number) + ")" +
				                         ConfigAccessErrorHint(error_number);
				return false;
			}
			// The ancestors are checked after the open, so they vouch for the file it names.
			const auto security = CheckSecureConfigFd(config.Get(), sources.config,
			                                          sources.owner_uid, sources.validation_root);
			if (!security.ok) {
				opened->error_message = security.error_message;
				return false;
			}
			opened->config = std::move(config);
			return true;
		}

		void OpenModel(const std::string &user, const RuntimeDescriptorSources &sources,
		               OpenedRuntimeDescriptors *opened) {
			const auto readiness = CheckUserModelReadiness(
			    sources.user_models_dir, user, sources.owner_uid, sources.validation_root);
			switch (readiness.status) {
				case UserModelStatus::kOk:
					break;
				case UserModelStatus::kNoModel:
				case UserModelStatus::kNoModelDirectory:
					opened->model_state = ModelState::kMissing;
					return;
				case UserModelStatus::kInvalidUser:
					opened->model_state   = ModelState::kInvalidUser;
					opened->model_message = std::string(kInvalidUserNameMessage);
					return;
				default:
					opened->model_state   = ModelState::kInvalidStorage;
					opened->model_message = readiness.error_message.empty()
					                            ? "Failed to validate user model file"
					                            : readiness.error_message;
					return;
			}

			howdy::native::ScopedFd model(open(readiness.path.c_str(), kOpenFlags));
			if (!model.Valid()) {
				const int error_number = errno;
				// Removed between the readiness walk and the open: as missing as before the walk.
				if (error_number == ENOENT) {
					opened->model_state = ModelState::kMissing;
					return;
				}
				opened->model_state = ModelState::kInvalidStorage;
				opened->model_message =
				    "Failed to open user model file: " + readiness.path.string() + " (" +
				    SystemErrorMessage(error_number) + ")";
				return;
			}
			// The walk checked the path; this checks the file the descriptor actually names.
			const auto security = CheckSecureRootOwnedFd(model.Get(), readiness.path,
			                                             kUserModelFileLabel, sources.owner_uid);
			if (!security.ok) {
				opened->model_state   = ModelState::kInvalidStorage;
				opened->model_message = security.error_message;
				return;
			}
			// No writer leaves an empty model: a first model is linked in whole at commit, and
			// commits never truncate. An empty file is damaged storage, such as a placeholder an
			// older version left behind, and is reported here by name rather than sent to
			// compare, which could only call it unparsable.
			struct stat model_stat{};
			if (fstat(model.Get(), &model_stat) != 0) {
				const int error_number = errno;
				opened->model_state    = ModelState::kInvalidStorage;
				opened->model_message =
				    "Failed to inspect user model file: " + readiness.path.string() + " (" +
				    SystemErrorMessage(error_number) + ")";
				return;
			}
			if (model_stat.st_size == 0) {
				opened->model_state   = ModelState::kInvalidStorage;
				opened->model_message = "User model file is empty: " + readiness.path.string();
				return;
			}
			opened->model_state = ModelState::kReady;
			opened->model       = std::move(model);
		}

	}  // namespace

	auto OpenRuntimeDescriptors(const std::string &user, const RuntimeDescriptorSources &sources)
	    -> OpenedRuntimeDescriptors {
		OpenedRuntimeDescriptors opened;
		if (!OpenConfig(sources, &opened)) {
			return opened;
		}
		OpenModel(user, sources, &opened);
		opened.ok = true;
		return opened;
	}

}  // namespace howdy::native::auth_helper
