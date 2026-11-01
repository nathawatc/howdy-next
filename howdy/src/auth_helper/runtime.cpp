#include "auth_helper/runtime.hpp"

#include "auth_helper/acl.hpp"
#include "auth_helper/runtime/internal.hpp"
#include "config/config_utils.hpp"
#include "config/runtime_paths.hpp"
#include "protocol/auth_helper_protocol.hpp"
#include "runtime/internal.hpp"
#include "storage/user_model_status.hpp"
#include "support/eintr.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

#include <unistd.h>

#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace howdy::native::auth_helper {
	namespace {
		// Creates runtime objects with the owner's group and their exact modes. The helper is
		// setuid but not setgid, so the runtime root and lock files would otherwise carry the
		// caller's group and a umask-narrowed mode until fchown/fchmod ran, and a kill in that
		// window left them failing validation. Slot directories also need ACLs and are only built
		// in a side directory. The previous umask and effective group are restored on return.
		class ScopedCreationIdentity {
		public:
			explicit ScopedCreationIdentity(gid_t owner_gid)
			    : previous_umask_(umask(0))
			    , previous_egid_(getegid())
			    , active_(setegid(owner_gid) == 0) {}

			ScopedCreationIdentity(const ScopedCreationIdentity &)                     = delete;
			auto operator=(const ScopedCreationIdentity &) -> ScopedCreationIdentity & = delete;
			ScopedCreationIdentity(ScopedCreationIdentity &&)                          = delete;
			auto operator=(ScopedCreationIdentity &&) -> ScopedCreationIdentity &      = delete;

			~ScopedCreationIdentity() {
				if (active_ && setegid(previous_egid_) != 0) {
					std::cerr << "Failed to restore the effective group after staging\n";
				}
				(void)umask(previous_umask_);
			}

			[[nodiscard]] auto Active() const -> bool {
				return active_;
			}

		private:
			mode_t previous_umask_;
			gid_t  previous_egid_;
			bool   active_;
		};
	}  // namespace

	namespace internal {

		auto PrepareRuntimeAuthFiles(const std::string &user, StagedIdentity identity,
		                             const RuntimeSources &sources, const AclOperations &operations)
		    -> std::optional<PreparedPaths> {
			const ScopedCreationIdentity creation_identity(identity.owner_gid);
			if (!creation_identity.Active()) {
				std::cerr << "Failed to adopt the owner group for runtime files\n";
				return std::nullopt;
			}
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
			if (RetryOnEintr([&] -> int {
				    return flock(allocation->Get(), LOCK_EX);
			    }) != 0) {
				return std::nullopt;
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
		return internal::PrepareRuntimeAuthFiles(
		    user, {.target_uid = identity.uid, .owner_uid = 0, .owner_gid = 0},
		    {
		        .runtime_root    = RuntimeRoot(),
		        .config          = howdy::native::ResolveConfigPath(),
		        .user_models_dir = howdy::native::ResolveUserModelsDir(),
		    },
		    ProductionAclOperations());
	}

}  // namespace howdy::native::auth_helper
