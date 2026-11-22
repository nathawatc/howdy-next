#include "auth_helper/runtime_descriptors.hpp"

#include "config/config_utils.hpp"
#include "protocol/runtime_descriptor_message.hpp"
#include "storage/user_model_store.hpp"
#include "support/error_message.hpp"
#include "support/scoped_fd.hpp"

#include <cerrno>
#include <string>
#include <utility>

#include <fcntl.h>

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
			const auto security =
			    CheckSecureConfigFd(config.Get(), sources.config, sources.config_owner_uid,
				                    sources.config_validation_root);
			if (!security.ok) {
				opened->error_message = security.error_message;
				return false;
			}
			opened->config = std::move(config);
			return true;
		}

		void OpenModel(const std::string &user, const RuntimeDescriptorSources &sources,
		               OpenedRuntimeDescriptors *opened) {
			auto result = sources.user_models.OpenRuntimeDescriptor(user);
			switch (result.status) {
				case UserModelDescriptorStatus::kReady:
					opened->model_state = ModelState::kReady;
					opened->model       = std::move(result.fd);
					return;
				case UserModelDescriptorStatus::kMissing:
					opened->model_state = ModelState::kMissing;
					return;
				case UserModelDescriptorStatus::kInvalidUser:
					opened->model_state   = ModelState::kInvalidUser;
					opened->model_message = std::move(result.error_message);
					return;
				case UserModelDescriptorStatus::kInvalidStorage:
					opened->model_state   = ModelState::kInvalidStorage;
					opened->model_message = std::move(result.error_message);
					return;
			}
			// Fail closed on a status outside the enum.
			opened->model_state   = ModelState::kInvalidStorage;
			opened->model_message = std::move(result.error_message);
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
