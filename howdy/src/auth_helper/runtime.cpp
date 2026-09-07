#include "auth_helper/runtime.hpp"

#include "auth_helper/acl.hpp"
#include "auth_helper/runtime/internal.hpp"
#include "config/config_utils.hpp"
#include "config/runtime_paths.hpp"
#include "protocol/auth_helper_protocol.hpp"
#include "runtime/internal.hpp"
#include "storage/user_model_status.hpp"

#include <cerrno>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

#include <sys/file.h>

namespace howdy::native::auth_helper {
	namespace internal {

		auto PrepareRuntimeAuthFiles(const std::string &user, StagedIdentity identity,
		                             const RuntimeSources &sources, const AclOperations &operations)
		    -> std::optional<PreparedPaths> {
			auto root = runtime_internal::OpenOrCreateRoot(sources.runtime_root, identity.owner_uid,
			                                               identity.owner_gid);
			if (!root.has_value()) {
				return std::nullopt;
			}
			const auto allocation_name = ".pam-" + std::to_string(identity.target_uid) + ".lock";
			auto       allocation      = runtime_internal::OpenRootOnlyLock(
			    root->Get(), allocation_name, sources.runtime_root / allocation_name, identity,
			    operations, true);
			if (!allocation.has_value()) {
				return std::nullopt;
			}
			while (flock(allocation->Get(), LOCK_EX) != 0) {
				if (errno != EINTR) {
					return std::nullopt;
				}
			}

			auto config_source = runtime_internal::OpenSourceFile(
			    sources.config, howdy::native::kConfigFileLabel, identity.owner_uid);
			if (!config_source.has_value()) {
				return std::nullopt;
			}
			const auto config_security = howdy::native::CheckSecureConfigFd(
			    config_source->fd.Get(), sources.config, identity.owner_uid);
			if (!config_security.ok) {
				std::cerr << config_security.error_message << "\n";
				return std::nullopt;
			}
			std::optional<std::filesystem::path> model_path;
			if (!SelectSourceModelPath(sources.user_models_dir, user, identity.owner_uid,
			                           model_path)) {
				return std::nullopt;
			}
			std::optional<runtime_internal::SourceFile> model_source;
			if (model_path.has_value()) {
				model_source = runtime_internal::OpenSourceFile(
				    *model_path, std::string(howdy::native::kUserModelFileLabel),
				    identity.owner_uid);
				if (!model_source.has_value()) {
					return std::nullopt;
				}
			}

			auto slots = runtime_internal::OpenSlots(root->Get(), sources, identity, operations);
			if (!slots.has_value()) {
				return std::nullopt;
			}
			if (auto fresh = runtime_internal::LeaseFreshSlot(*slots, *config_source, model_source,
			                                                  user, identity, operations);
			    fresh.has_value()) {
				return fresh;
			}
			return runtime_internal::RefreshAvailableSlot(*slots, *config_source, model_source,
			                                              user, identity, operations);
		}

	}  // namespace internal

	auto RuntimeRoot() -> std::filesystem::path {
		return auth_helper_protocol::PreparedRuntimeRoot();
	}

	auto PrepareRuntimeAuthFiles(const std::string &user, RuntimeIdentity identity)
	    -> std::optional<PreparedPaths> {
		(void)identity.gid;
		return internal::PrepareRuntimeAuthFiles(
		    user, {.target_uid = identity.uid, .owner_uid = 0, .owner_gid = 0},
		    {.runtime_root    = RuntimeRoot(),
		     .config          = howdy::native::ResolveConfigPath(),
		     .user_models_dir = howdy::native::ResolveUserModelsDir()},
		    ProductionAclOperations());
	}

}  // namespace howdy::native::auth_helper
