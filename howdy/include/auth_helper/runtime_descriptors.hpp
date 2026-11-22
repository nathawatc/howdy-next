#pragma once

#include "protocol/runtime_descriptor_message.hpp"
#include "storage/user_model_store.hpp"
#include "support/file_security/validation_root.hpp"
#include "support/scoped_fd.hpp"

#include <filesystem>
#include <string>

#include <sys/types.h>

namespace howdy::native::auth_helper {

	struct RuntimeDescriptorSources {
		std::filesystem::path                  config;
		UserModelStore                         user_models;
		uid_t                                  config_owner_uid = 0;
		file_security_internal::ValidationRoot config_validation_root;
	};

	struct OpenedRuntimeDescriptors {
		// False when the config could not be opened securely; nothing else is then set.
		bool        ok = false;
		std::string error_message;

		howdy::native::ScopedFd                config;
		runtime_descriptor_message::ModelState model_state =
		    runtime_descriptor_message::ModelState::kInvalidStorage;
		// Open exactly when model_state is kReady.
		howdy::native::ScopedFd model;
		// Why the model is not ready, for the log; empty when it is ready or simply missing.
		std::string model_message;
	};

	// Opens the runtime config and the user's model read-only and close-on-exec, with the same
	// checks the path loaders use: the config by path with O_NOFOLLOW and then its ancestors,
	// the model through the user-model store's readiness walk and descriptor validator. Shared by
	// the setuid helper's `open` and by a root PAM module, so both hand compare the same thing.
	auto OpenRuntimeDescriptors(const std::string &user, const RuntimeDescriptorSources &sources)
	    -> OpenedRuntimeDescriptors;

}  // namespace howdy::native::auth_helper
