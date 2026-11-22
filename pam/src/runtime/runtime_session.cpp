#include "runtime/runtime_session.hpp"

#include "auth_helper/runtime_descriptors.hpp"
#include "config/runtime_config_loader.hpp"
#include "protocol/runtime_descriptor_message.hpp"
#include "runtime/auth_helper_process.hpp"
#include "storage/user_model_path.hpp"
#include "storage/user_model_store.hpp"
#include "support/bit_flags.hpp"
#include "support/file_security.hpp"

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

#include <sys/syslog.h>
#include <sys/types.h>

namespace {

	using howdy::native::runtime_descriptor_message::ModelState;

	auto OpenInProcessDependency(void *context, std::string_view username,
	                             const howdy::pam::RuntimeSessionPaths &paths,
	                             howdy::pam::RuntimeDescriptorSet      *descriptors) -> bool {
		(void)context;
		auto opened = howdy::native::auth_helper::OpenRuntimeDescriptors(
		    std::string(username), {
		                               .config           = paths.config_path,
		                               .user_models      = howdy::native::UserModelStore({
		                                   .models_dir = paths.user_models_dir,
		                                   .owner_uid  = 0,
		                               }),
		                               .config_owner_uid = 0,
		                           });
		if (!opened.ok) {
			syslog(LOG_AUTHPRIV | LOG_ERR, "pam_howdy: %s", opened.error_message.c_str());
			return false;
		}
		descriptors->config        = std::move(opened.config);
		descriptors->model_state   = opened.model_state;
		descriptors->model         = std::move(opened.model);
		descriptors->model_message = std::move(opened.model_message);
		return true;
	}

	auto OpenThroughHelperDependency(void *context, std::string_view username,
	                                 howdy::pam::RuntimeDescriptorSet *descriptors) -> bool {
		(void)context;
		return howdy::pam::auth_helper_process::OpenRuntimeDescriptors(username, descriptors);
	}

	auto LoadRuntimeConfigDependency(void *context, int config_fd,
	                                 const std::filesystem::path &display_path, uid_t owner_uid)
	    -> howdy::native::RuntimeConfigLoadResult {
		(void)context;
		return howdy::native::LoadRuntimeConfigFromFd(config_fd, display_path, owner_uid);
	}

	auto EffectiveUidDependency(void *context) -> uid_t {
		(void)context;
		return geteuid();
	}

	// What compare and PAM may trust in a descriptor from either source: close-on-exec, so no
	// other child of the application inherits it, and a read-only, regular, singly linked file
	// that only `owner_uid` can write.
	auto CheckDescriptor(int fd, std::string_view label, const std::filesystem::path &display_path,
	                     uid_t owner_uid) -> std::optional<std::string> {
		const int descriptor_flags = fcntl(fd, F_GETFD);
		if (descriptor_flags < 0 || !howdy::native::HasAnyBits(descriptor_flags, FD_CLOEXEC)) {
			return std::string(label) + " is not close-on-exec: " + display_path.string();
		}
		if (const auto read_only = howdy::native::CheckReadOnlyFd(fd, display_path, label);
		    !read_only.ok) {
			return read_only.error_message;
		}
		if (const auto security =
		        howdy::native::CheckSecureRootOwnedFd(fd, display_path, label, owner_uid);
		    !security.ok) {
			return security.error_message;
		}
		return std::nullopt;
	}

	// `paths` and `username` only name the descriptors' sources in messages.
	auto CheckDescriptorSet(const howdy::pam::RuntimeDescriptorSet &descriptors,
	                        const howdy::pam::RuntimeSessionPaths &paths, std::string_view username,
	                        uid_t owner_uid) -> std::optional<std::string> {
		if (!descriptors.config.Valid()) {
			return std::string("Runtime config descriptor is missing");
		}
		const bool ready = descriptors.model_state == ModelState::kReady;
		if (ready != descriptors.model.Valid()) {
			return std::string(ready ? "Ready user model has no descriptor"
			                         : "User model descriptor came with a model that is not ready");
		}
		if (auto error = CheckDescriptor(descriptors.config.Get(), "Runtime config descriptor",
		                                 paths.config_path, owner_uid)) {
			return error;
		}
		if (ready) {
			const std::filesystem::path models_dir(paths.user_models_dir);
			return CheckDescriptor(
			    descriptors.model.Get(), "User model descriptor",
			    howdy::native::ResolveUserModelPath(models_dir, username).value_or(models_dir),
			    owner_uid);
		}
		return std::nullopt;
	}

}  // namespace

namespace howdy::pam {

	RuntimeSession::RuntimeSession(RuntimeSessionPaths paths, uid_t trusted_owner_uid,
	                               RuntimeSessionOperations operations)
	    : paths_(std::move(paths))
	    , trusted_owner_uid_(trusted_owner_uid)
	    , operations_(operations) {}

	RuntimeSession::~RuntimeSession() = default;

	auto RuntimeSession::LoadForUser(std::string_view username) -> RuntimeSessionLoadResult {
		if (load_started_) {
			return {.status = RuntimeSessionLoadStatus::kAlreadyLoaded};
		}
		load_started_ = true;

		RuntimeDescriptorSet opened;
		const bool           in_process = operations_.EffectiveUid() == 0;
		const bool opened_ok = in_process ? operations_.OpenInProcess(username, paths_, &opened)
		                                  : operations_.OpenThroughHelper(username, &opened);
		if (!opened_ok) {
			return {.status = RuntimeSessionLoadStatus::kOpenFailed};
		}
		if (auto error = CheckDescriptorSet(opened, paths_, username, trusted_owner_uid_)) {
			return {
			    .status        = RuntimeSessionLoadStatus::kOpenFailed,
			    .error_message = std::move(*error),
			};
		}

		auto config_result = operations_.LoadRuntimeConfig(opened.config.Get(), paths_.config_path,
		                                                   trusted_owner_uid_);
		const bool config_ok =
		    config_result.status == howdy::native::RuntimeConfigLoadStatus::kOk &&
		    config_result.config.has_value();
		if (config_ok) {
			descriptors_ = std::move(opened);
		}
		return {
		    .status        = config_ok ? RuntimeSessionLoadStatus::kOk
			                           : RuntimeSessionLoadStatus::kConfigLoadFailed,
		    .config_result = std::move(config_result),
		};
	}

	auto RuntimeSession::ModelState() const
	    -> howdy::native::runtime_descriptor_message::ModelState {
		return descriptors_.model_state;
	}

	auto RuntimeSession::ModelMessage() const -> const std::string & {
		return descriptors_.model_message;
	}

	auto RuntimeSession::ConfigFd() const -> int {
		return descriptors_.config.Get();
	}

	auto RuntimeSession::ModelFd() const -> int {
		return descriptors_.model.Get();
	}

	auto ProductionRuntimeSessionOperations() -> RuntimeSessionOperations {
		auto operations = RuntimeSessionOperations::Create(
		    nullptr, OpenInProcessDependency, OpenThroughHelperDependency,
		    LoadRuntimeConfigDependency, EffectiveUidDependency);
		if (!operations.has_value()) {
			throw std::logic_error("Failed to create production runtime session operations");
		}
		return *operations;
	}

}  // namespace howdy::pam
