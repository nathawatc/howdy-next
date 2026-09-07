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

		auto prepare_runtime_auth_files(const std::string &user, StagedIdentity identity,
		                                const RuntimeSources &sources,
		                                const AclOperations  &operations)
		    -> std::optional<PreparedPaths> {
			auto root = runtime_internal::open_or_create_root(
			    sources.runtime_root, identity.owner_uid, identity.owner_gid);
			if (!root.has_value()) {
				return std::nullopt;
			}
			const auto allocation_name = ".pam-" + std::to_string(identity.target_uid) + ".lock";
			auto       allocation      = runtime_internal::open_root_only_lock(
			    root->get(), allocation_name, sources.runtime_root / allocation_name, identity,
			    operations, true);
			if (!allocation.has_value()) {
				return std::nullopt;
			}
			while (flock(allocation->get(), LOCK_EX) != 0) {
				if (errno != EINTR) {
					return std::nullopt;
				}
			}

			auto config_source = runtime_internal::open_source_file(
			    sources.config, howdy::native::kConfigFileLabel, identity.owner_uid);
			if (!config_source.has_value()) {
				return std::nullopt;
			}
			const auto config_security = howdy::native::check_secure_config_fd(
			    config_source->fd.get(), sources.config, identity.owner_uid);
			if (!config_security.ok) {
				std::cerr << config_security.error_message << "\n";
				return std::nullopt;
			}
			std::optional<std::filesystem::path> model_path;
			if (!select_source_model_path(sources.user_models_dir, user, identity.owner_uid,
			                              model_path)) {
				return std::nullopt;
			}
			std::optional<runtime_internal::SourceFile> model_source;
			if (model_path.has_value()) {
				model_source = runtime_internal::open_source_file(
				    *model_path, std::string(howdy::native::kUserModelFileLabel),
				    identity.owner_uid);
				if (!model_source.has_value()) {
					return std::nullopt;
				}
			}

			auto slots = runtime_internal::open_slots(root->get(), sources, identity, operations);
			if (!slots.has_value()) {
				return std::nullopt;
			}
			if (auto fresh = runtime_internal::lease_fresh_slot(
			        *slots, *config_source, model_source, user, identity, operations);
			    fresh.has_value()) {
				return fresh;
			}
			return runtime_internal::refresh_available_slot(*slots, *config_source, model_source,
			                                                user, identity, operations);
		}

	}  // namespace internal

	auto runtime_root() -> std::filesystem::path {
		return auth_helper_protocol::prepared_runtime_root();
	}

	auto prepare_runtime_auth_files(const std::string &user, RuntimeIdentity identity)
	    -> std::optional<PreparedPaths> {
		(void)identity.gid;
		return internal::prepare_runtime_auth_files(
		    user, {.target_uid = identity.uid, .owner_uid = 0, .owner_gid = 0},
		    {.runtime_root    = runtime_root(),
		     .config          = howdy::native::resolve_config_path(),
		     .user_models_dir = howdy::native::resolve_user_models_dir()},
		    production_acl_operations());
	}

}  // namespace howdy::native::auth_helper
