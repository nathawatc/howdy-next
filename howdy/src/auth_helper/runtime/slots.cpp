#include "auth_helper/acl.hpp"
#include "auth_helper/runtime.hpp"
#include "auth_helper/runtime/internal.hpp"
#include "internal.hpp"
#include "protocol/auth_helper_protocol.hpp"
#include "storage/staged_runtime_policy.hpp"
#include "support/fd_io.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>

#include <sys/file.h>
#include <sys/stat.h>

namespace howdy::native::auth_helper {
	namespace {
		using auth_helper_protocol::RuntimeGenerationSlot;
		using internal::RuntimeSources;
		using internal::StagedIdentity;

		auto log_slots_errno_failure(std::string_view operation, const std::filesystem::path &path,
		                             int error_number) -> bool {
			std::cerr << "Failed to " << operation << " '" << path
			          << "': " << std::strerror(error_number) << "\n";
			return false;
		}

		auto validate_regular(const struct stat &stat_, StagedIdentity identity,
		                      StagedRuntimeRole role) -> bool {
			const auto policy = staged_runtime_policy(role);
			return policy.exact_link_count.has_value() && S_ISREG(stat_.st_mode) &&
			       stat_.st_uid == identity.owner_uid && stat_.st_gid == identity.owner_gid &&
			       (stat_.st_mode & 07777) == policy.mode &&
			       stat_.st_nlink == *policy.exact_link_count;
		}

		auto validate_directory(const struct stat &stat_, StagedIdentity identity,
		                        StagedRuntimeRole role) -> bool {
			const auto policy = staged_runtime_policy(role);
			return S_ISDIR(stat_.st_mode) && stat_.st_uid == identity.owner_uid &&
			       stat_.st_gid == identity.owner_gid && (stat_.st_mode & 07777) == policy.mode;
		}

		auto set_owner(int fd, StagedIdentity identity) -> bool {
			struct stat stat_{};
			if (fstat(fd, &stat_) != 0) {
				return false;
			}
			return (stat_.st_uid == identity.owner_uid && stat_.st_gid == identity.owner_gid) ||
			       fchown(fd, identity.owner_uid, identity.owner_gid) == 0;
		}

		auto apply_private_acl(int fd, const std::filesystem::path &path, StagedIdentity identity,
		                       StagedRuntimeRole role, const AclOperations &operations) -> bool {
			const auto policy = staged_runtime_policy(role);
			return set_owner(fd, identity) && fchmod(fd, 0600) == 0 &&
			       set_persistent_acl_with_operations(fd, path, identity.target_uid, policy.acl,
			                                          operations) &&
			       fchmod(fd, policy.mode) == 0 &&
			       verify_persistent_acl_with_operations(fd, path, identity.target_uid, policy.acl,
			                                             operations);
		}

		auto apply_owner_only_acl(int fd, const std::filesystem::path &path, StagedRuntimeRole role,
		                          const AclOperations &operations) -> bool {
			const auto policy = staged_runtime_policy(role);
			return fchmod(fd, policy.mode) == 0 &&
			       set_persistent_acl_with_operations(fd, path, uid_t{0}, policy.acl, operations) &&
			       fchmod(fd, policy.mode) == 0 &&
			       verify_persistent_acl_with_operations(fd, path, uid_t{0}, policy.acl,
			                                             operations);
		}

		auto open_slot_directory(int root_fd, const std::filesystem::path &root,
		                         RuntimeGenerationSlot generation, StagedIdentity identity,
		                         const AclOperations &operations, bool create)
		    -> std::optional<runtime_internal::UniqueFd> {
			const auto name = auth_helper_protocol::prepared_runtime_generation_name(
			    identity.target_uid, generation);
			bool created = false;
			if (create && mkdirat(root_fd, name.c_str(), 0700) == 0) {
				created = true;
			} else if (create && errno != EEXIST) {
				return std::nullopt;
			}
			runtime_internal::UniqueFd fd(
			    openat(root_fd, name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
			if (fd.get() < 0) {
				return std::nullopt;
			}
			const auto path = root / name;
			if (created && !apply_private_acl(fd.get(), path, identity,
			                                  StagedRuntimeRole::kSharedDirectory, operations)) {
				return std::nullopt;
			}
			struct stat stat_{};
			if (fstat(fd.get(), &stat_) != 0 ||
			    !validate_directory(stat_, identity, StagedRuntimeRole::kSharedDirectory) ||
			    !verify_persistent_acl_with_operations(
			        fd.get(), path, identity.target_uid,
			        staged_runtime_policy(StagedRuntimeRole::kSharedDirectory).acl, operations)) {
				std::cerr << "Runtime slot failed strict validation: " << path << "\n";
				return std::nullopt;
			}
			return fd;
		}

		auto open_models_directory(int slot_fd, const std::filesystem::path &slot_path,
		                           StagedIdentity identity, const AclOperations &operations,
		                           bool create) -> std::optional<runtime_internal::UniqueFd> {
			bool created = false;
			if (create && mkdirat(slot_fd, auth_helper_protocol::kPreparedUserModelsDirectoryName,
			                      0700) == 0) {
				created = true;
			} else if (create && errno != EEXIST) {
				return std::nullopt;
			}
			runtime_internal::UniqueFd fd(
			    openat(slot_fd, auth_helper_protocol::kPreparedUserModelsDirectoryName,
			           O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
			if (fd.get() < 0) {
				return std::nullopt;
			}
			const auto path = auth_helper_protocol::prepared_user_models_dir(slot_path);
			if (created && !apply_private_acl(fd.get(), path, identity,
			                                  StagedRuntimeRole::kSharedDirectory, operations)) {
				return std::nullopt;
			}
			struct stat stat_{};
			if (fstat(fd.get(), &stat_) != 0 ||
			    !validate_directory(stat_, identity, StagedRuntimeRole::kSharedDirectory) ||
			    !verify_persistent_acl_with_operations(
			        fd.get(), path, identity.target_uid,
			        staged_runtime_policy(StagedRuntimeRole::kSharedDirectory).acl, operations)) {
				return std::nullopt;
			}
			return fd;
		}

		auto open_config_file(int slot_fd, const std::filesystem::path &slot_path,
		                      StagedIdentity identity, const AclOperations &operations, bool create)
		    -> std::optional<runtime_internal::UniqueFd> {
			bool created = false;
			int  raw_fd  = openat(slot_fd, auth_helper_protocol::kPreparedConfigFileName,
			                      O_RDWR | O_CLOEXEC | O_NOFOLLOW);
			if (create && raw_fd < 0 && errno == ENOENT) {
				raw_fd  = openat(slot_fd, auth_helper_protocol::kPreparedConfigFileName,
				                 O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
				created = raw_fd >= 0;
			}
			runtime_internal::UniqueFd fd(raw_fd);
			if (fd.get() < 0) {
				return std::nullopt;
			}
			const auto path = auth_helper_protocol::prepared_config_path(slot_path);
			if (created && !apply_private_acl(fd.get(), path, identity, StagedRuntimeRole::kConfig,
			                                  operations)) {
				return std::nullopt;
			}
			struct stat stat_{};
			if (fstat(fd.get(), &stat_) != 0 ||
			    !validate_regular(stat_, identity, StagedRuntimeRole::kConfig) ||
			    !verify_persistent_acl_with_operations(
			        fd.get(), path, identity.target_uid,
			        staged_runtime_policy(StagedRuntimeRole::kConfig).acl, operations)) {
				return std::nullopt;
			}
			return fd;
		}

		auto open_model_backing(int slot_fd, const std::filesystem::path &slot_path,
		                        StagedIdentity identity, const AclOperations &operations,
		                        bool create) -> std::optional<runtime_internal::UniqueFd> {
			bool created = false;
			int  raw_fd  = openat(slot_fd, auth_helper_protocol::kPreparedModelBackingFileName,
			                      O_RDWR | O_CLOEXEC | O_NOFOLLOW);
			if (create && raw_fd < 0 && errno == ENOENT) {
				raw_fd  = openat(slot_fd, auth_helper_protocol::kPreparedModelBackingFileName,
				                 O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
				created = raw_fd >= 0;
			}
			runtime_internal::UniqueFd fd(raw_fd);
			if (fd.get() < 0) {
				return std::nullopt;
			}
			const auto path = slot_path / auth_helper_protocol::kPreparedModelBackingFileName;
			if (created && (!set_owner(fd.get(), identity) ||
			                !apply_owner_only_acl(fd.get(), path, StagedRuntimeRole::kAbsentModel,
			                                      operations))) {
				return std::nullopt;
			}
			return fd;
		}

		auto models_directory_contains_only(int models_fd, std::string_view expected_name,
		                                    bool expected_present) -> bool {
			runtime_internal::UniqueFd duplicate(
			    openat(models_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
			if (duplicate.get() < 0) {
				return false;
			}
			DIR *directory = fdopendir(duplicate.release());
			if (directory == nullptr) {
				return false;
			}
			bool found = false;
			errno      = 0;
			while (dirent *entry = readdir(directory)) {
				const std::string_view name(entry->d_name);
				if (name == "." || name == "..") {
					continue;
				}
				if (name != expected_name || found) {
					(void)closedir(directory);
					return false;
				}
				found = true;
			}
			const bool ok = errno == 0 && found == expected_present;
			(void)closedir(directory);
			return ok;
		}

		enum class ModelState : std::uint8_t {
			kAbsent,
			kPresent,
			kInvalid,
		};

		enum class SlotInitialization : std::uint8_t {
			kEmpty,
			kComplete,
			kInvalid,
		};

		auto slot_initialization(int slot_fd) -> SlotInitialization {
			std::array<bool, 3>  present{};
			constexpr std::array names = {auth_helper_protocol::kPreparedConfigFileName,
			                              auth_helper_protocol::kPreparedUserModelsDirectoryName,
			                              auth_helper_protocol::kPreparedModelBackingFileName};
			for (std::size_t index = 0; index < names.size(); ++index) {
				struct stat stat_{};
				if (fstatat(slot_fd, names[index], &stat_, AT_SYMLINK_NOFOLLOW) == 0) {
					present[index] = true;
				} else if (errno != ENOENT) {
					return SlotInitialization::kInvalid;
				}
			}
			if (std::ranges::all_of(present, std::identity{})) {
				return SlotInitialization::kComplete;
			}
			return std::ranges::none_of(present, std::identity{}) ? SlotInitialization::kEmpty
			                                                      : SlotInitialization::kInvalid;
		}

		auto inspect_model_state(int models_fd, const std::filesystem::path &slot_path,
		                         const std::string &user, int backing_fd, StagedIdentity identity,
		                         const AclOperations &operations) -> ModelState {
			struct stat backing_stat{};
			if (fstat(backing_fd, &backing_stat) != 0 || !S_ISREG(backing_stat.st_mode) ||
			    backing_stat.st_uid != identity.owner_uid ||
			    backing_stat.st_gid != identity.owner_gid) {
				return ModelState::kInvalid;
			}
			const auto backing_path =
			    slot_path / auth_helper_protocol::kPreparedModelBackingFileName;
			const auto model_path =
			    auth_helper_protocol::prepared_user_models_dir(slot_path) / (user + ".dat");
			struct stat visible_stat{};
			const bool  visible = fstatat(models_fd, (user + ".dat").c_str(), &visible_stat,
			                              AT_SYMLINK_NOFOLLOW) == 0;
			if (!visible && errno != ENOENT) {
				return ModelState::kInvalid;
			}
			if (!visible) {
				const auto policy = staged_runtime_policy(StagedRuntimeRole::kAbsentModel);
				return validate_regular(backing_stat, identity, StagedRuntimeRole::kAbsentModel) &&
				               verify_persistent_acl_with_operations(
				                   backing_fd, backing_path, uid_t{0}, policy.acl, operations) &&
				               models_directory_contains_only(models_fd, user + ".dat", false)
				           ? ModelState::kAbsent
				           : ModelState::kInvalid;
			}
			return validate_regular(backing_stat, identity, StagedRuntimeRole::kPresentModel) &&
			               validate_regular(visible_stat, identity,
			                                StagedRuntimeRole::kPresentModel) &&
			               backing_stat.st_dev == visible_stat.st_dev &&
			               backing_stat.st_ino == visible_stat.st_ino &&
			               verify_persistent_acl_with_operations(
			                   backing_fd, model_path, identity.target_uid,
			                   staged_runtime_policy(StagedRuntimeRole::kPresentModel).acl,
			                   operations) &&
			               models_directory_contains_only(models_fd, user + ".dat", true)
			           ? ModelState::kPresent
			           : ModelState::kInvalid;
		}

		auto slot_is_fresh(runtime_internal::Slot                            &slot,
		                   const runtime_internal::SourceFile                &config_source,
		                   const std::optional<runtime_internal::SourceFile> &model_source,
		                   const std::string &user, StagedIdentity identity,
		                   const AclOperations &operations) -> bool {
			auto models =
			    open_models_directory(slot.dir_fd.get(), slot.path, identity, operations, false);
			auto config =
			    open_config_file(slot.dir_fd.get(), slot.path, identity, operations, false);
			auto backing =
			    open_model_backing(slot.dir_fd.get(), slot.path, identity, operations, false);
			if (!models.has_value() || !config.has_value() || !backing.has_value()) {
				return false;
			}
			const auto model_state = inspect_model_state(models->get(), slot.path, user,
			                                             backing->get(), identity, operations);
			if (model_state == ModelState::kInvalid ||
			    !runtime_internal::compare_files(config_source.fd.get(), config->get()) ||
			    !runtime_internal::source_unchanged(config_source)) {
				return false;
			}
			if (!model_source.has_value()) {
				return model_state == ModelState::kAbsent;
			}
			return model_state == ModelState::kPresent &&
			       runtime_internal::compare_files(model_source->fd.get(), backing->get()) &&
			       runtime_internal::source_unchanged(*model_source);
		}

		auto update_slot(runtime_internal::Slot                            &slot,
		                 const runtime_internal::SourceFile                &config_source,
		                 const std::optional<runtime_internal::SourceFile> &model_source,
		                 const std::string &user, StagedIdentity identity,
		                 const AclOperations &operations) -> bool {
			const auto initialization = slot_initialization(slot.dir_fd.get());
			if (initialization == SlotInitialization::kInvalid) {
				std::cerr << "Persistent runtime objects are incomplete in " << slot.path << "\n";
				return false;
			}
			const bool create = initialization == SlotInitialization::kEmpty;
			auto       models =
			    open_models_directory(slot.dir_fd.get(), slot.path, identity, operations, create);
			auto config =
			    open_config_file(slot.dir_fd.get(), slot.path, identity, operations, create);
			auto backing =
			    open_model_backing(slot.dir_fd.get(), slot.path, identity, operations, create);
			if (!models.has_value() || !config.has_value() || !backing.has_value()) {
				std::cerr << "Failed to open persistent runtime objects in " << slot.path
				          << " (models=" << models.has_value() << ", config=" << config.has_value()
				          << ", backing=" << backing.has_value() << ")\n";
				return false;
			}
			const auto state = inspect_model_state(models->get(), slot.path, user, backing->get(),
			                                       identity, operations);
			if (state == ModelState::kInvalid) {
				std::cerr << "Persistent model state failed strict validation in " << slot.path
				          << "\n";
				return false;
			}
			if (!runtime_internal::copy_source_to_open_file(config_source, config->get()) ||
			    !apply_private_acl(config->get(),
			                       auth_helper_protocol::prepared_config_path(slot.path), identity,
			                       StagedRuntimeRole::kConfig, operations)) {
				std::cerr << "Failed to update persistent config in " << slot.path << "\n";
				return false;
			}

			const std::string model_name = user + ".dat";
			const auto        backing_path =
			    slot.path / auth_helper_protocol::kPreparedModelBackingFileName;
			const auto visible_path =
			    auth_helper_protocol::prepared_user_models_dir(slot.path) / model_name;
			if (model_source.has_value()) {
				if (!runtime_internal::copy_source_to_open_file(*model_source, backing->get()) ||
				    !apply_private_acl(backing->get(), visible_path, identity,
				                       StagedRuntimeRole::kPresentModel, operations)) {
					std::cerr << "Failed to update persistent model in " << slot.path << "\n";
					return false;
				}
				if (state == ModelState::kAbsent &&
				    linkat(slot.dir_fd.get(), auth_helper_protocol::kPreparedModelBackingFileName,
				           models->get(), model_name.c_str(), 0) != 0) {
					return log_slots_errno_failure("link visible model", visible_path, errno);
				}
			} else {
				if (state == ModelState::kPresent &&
				    unlinkat(models->get(), model_name.c_str(), 0) != 0) {
					return false;
				}
				if (ftruncate(backing->get(), 0) != 0 || !howdy::native::sync_fd(backing->get()) ||
				    !apply_owner_only_acl(backing->get(), backing_path,
				                          StagedRuntimeRole::kAbsentModel, operations)) {
					return false;
				}
			}
			if (!howdy::native::sync_fd(models->get()) ||
			    !howdy::native::sync_fd(slot.dir_fd.get())) {
				std::cerr << "Failed to sync persistent runtime directories in " << slot.path
				          << "\n";
				return false;
			}
			const bool valid = inspect_model_state(models->get(), slot.path, user, backing->get(),
			                                       identity, operations) != ModelState::kInvalid;
			if (!valid) {
				std::cerr << "Updated model state failed strict validation in " << slot.path
				          << "\n";
			}
			return valid && runtime_internal::source_unchanged(config_source) &&
			       (!model_source.has_value() || runtime_internal::source_unchanged(*model_source));
		}

		auto open_slot(int root_fd, const std::filesystem::path &root,
		               RuntimeGenerationSlot generation, StagedIdentity identity,
		               const AclOperations &operations, bool create)
		    -> std::optional<runtime_internal::Slot> {
			const auto lock_name = auth_helper_protocol::prepared_runtime_generation_lock_name(
			    identity.target_uid, generation);
			auto lock = runtime_internal::open_root_only_lock(root_fd, lock_name, root / lock_name,
			                                                  identity, operations, create);
			if (!lock.has_value()) {
				return std::nullopt;
			}
			auto directory =
			    open_slot_directory(root_fd, root, generation, identity, operations, create);
			if (!directory.has_value()) {
				return std::nullopt;
			}
			return runtime_internal::Slot{.path =
			                                  auth_helper_protocol::prepared_runtime_generation_dir(
			                                      root, identity.target_uid, generation),
			                              .lock_fd = std::move(*lock),
			                              .dir_fd  = std::move(*directory)};
		}

		auto prepared_paths(runtime_internal::Slot &slot) -> std::optional<PreparedPaths> {
			const auto lock_path = std::filesystem::path(slot.path.string() + ".lock");
			runtime_internal::UniqueFd lease(
			    open(lock_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
			struct stat writable_stat{};
			struct stat lease_stat{};
			const auto  policy = staged_runtime_policy(StagedRuntimeRole::kLock);
			if (lease.get() < 0 || fstat(slot.lock_fd.get(), &writable_stat) != 0 ||
			    fstat(lease.get(), &lease_stat) != 0 || writable_stat.st_dev != lease_stat.st_dev ||
			    writable_stat.st_ino != lease_stat.st_ino || !S_ISREG(lease_stat.st_mode) ||
			    (lease_stat.st_mode & 07777) != policy.mode ||
			    !policy.exact_link_count.has_value() ||
			    lease_stat.st_nlink != *policy.exact_link_count ||
			    flock(lease.get(), LOCK_SH | LOCK_NB) != 0) {
				return std::nullopt;
			}
			return PreparedPaths{
			    .runtime_dir     = slot.path,
			    .config_path     = auth_helper_protocol::prepared_config_path(slot.path),
			    .user_models_dir = auth_helper_protocol::prepared_user_models_dir(slot.path),
			    .lease_fd        = lease.release()};
		}
	}  // namespace

	namespace runtime_internal {

		auto open_or_create_root(const std::filesystem::path &path, uid_t owner_uid,
		                         gid_t owner_gid) -> std::optional<UniqueFd> {
			if (!internal::validate_runtime_root(path, owner_uid, owner_gid)) {
				return std::nullopt;
			}
			UniqueFd fd(open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
			if (fd.get() < 0) {
				return std::nullopt;
			}
			struct stat stat_{};
			const auto  policy = staged_runtime_policy(StagedRuntimeRole::kRuntimeRoot);
			if (fstat(fd.get(), &stat_) != 0 || !S_ISDIR(stat_.st_mode) ||
			    stat_.st_uid != owner_uid || stat_.st_gid != owner_gid ||
			    (stat_.st_mode & 07777) != policy.mode) {
				return std::nullopt;
			}
			return fd;
		}

		auto open_root_only_lock(int root_fd, const std::string &name,
		                         const std::filesystem::path &display_path, StagedIdentity identity,
		                         const AclOperations &operations, bool create)
		    -> std::optional<UniqueFd> {
			bool created = false;
			int  raw_fd  = openat(root_fd, name.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
			if (create && raw_fd < 0 && errno == ENOENT) {
				raw_fd  = openat(root_fd, name.c_str(),
				                 O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
				created = raw_fd >= 0;
			}
			UniqueFd fd(raw_fd);
			if (fd.get() < 0) {
				log_slots_errno_failure("open runtime lock", display_path, errno);
				return std::nullopt;
			}
			if (created && (!set_owner(fd.get(), identity) ||
			                !apply_owner_only_acl(fd.get(), display_path, StagedRuntimeRole::kLock,
			                                      operations))) {
				return std::nullopt;
			}
			struct stat stat_{};
			const auto  policy = staged_runtime_policy(StagedRuntimeRole::kLock);
			if (fstat(fd.get(), &stat_) != 0 ||
			    !validate_regular(stat_, identity, StagedRuntimeRole::kLock) ||
			    !verify_persistent_acl_with_operations(fd.get(), display_path, uid_t{0}, policy.acl,
			                                           operations)) {
				std::cerr << "Runtime lock failed strict validation: " << display_path << "\n";
				return std::nullopt;
			}
			return fd;
		}

		auto open_slots(int root_fd, const RuntimeSources &sources, StagedIdentity identity,
		                const AclOperations &operations) -> std::optional<SlotSet> {
			constexpr std::array generations = {RuntimeGenerationSlot::kSlot0,
			                                    RuntimeGenerationSlot::kSlot1};
			SlotSet              slots;
			bool                 any_valid = false;
			for (std::size_t index = 0; index < slots.size(); ++index) {
				slots[index] = open_slot(root_fd, sources.runtime_root, generations[index],
				                         identity, operations, true);
				any_valid    = any_valid || slots[index].has_value();
			}
			return any_valid ? std::optional<SlotSet>{std::move(slots)} : std::nullopt;
		}

		auto lease_fresh_slot(SlotSet &slots, const SourceFile &config_source,
		                      const std::optional<SourceFile> &model_source,
		                      const std::string &user, StagedIdentity identity,
		                      const AclOperations &operations) -> std::optional<PreparedPaths> {
			for (auto &slot : slots) {
				if (!slot.has_value() || flock(slot->lock_fd.get(), LOCK_SH | LOCK_NB) != 0) {
					continue;
				}
				if (slot_is_fresh(*slot, config_source, model_source, user, identity, operations)) {
					auto prepared = prepared_paths(*slot);
					if (prepared.has_value()) {
						return prepared;
					}
				}
				(void)flock(slot->lock_fd.get(), LOCK_UN);
			}
			return std::nullopt;
		}

		auto refresh_available_slot(SlotSet &slots, const SourceFile &config_source,
		                            const std::optional<SourceFile> &model_source,
		                            const std::string &user, StagedIdentity identity,
		                            const AclOperations &operations)
		    -> std::optional<PreparedPaths> {
			for (auto &slot : slots) {
				if (!slot.has_value() || flock(slot->lock_fd.get(), LOCK_EX | LOCK_NB) != 0) {
					continue;
				}
				if (!update_slot(*slot, config_source, model_source, user, identity, operations)) {
					slot->lock_fd.reset();
					continue;
				}
				if (flock(slot->lock_fd.get(), LOCK_SH | LOCK_NB) != 0) {
					slot->lock_fd.reset();
					continue;
				}
				auto prepared = prepared_paths(*slot);
				if (prepared.has_value()) {
					return prepared;
				}
			}
			std::cerr << "No usable auth-helper runtime slot\n";
			return std::nullopt;
		}

	}  // namespace runtime_internal

	namespace internal {

		auto validate_runtime_root(const std::filesystem::path &path, uid_t owner_uid,
		                           gid_t owner_gid) -> bool {
			const auto policy  = staged_runtime_policy(StagedRuntimeRole::kRuntimeRoot);
			const bool created = mkdir(path.c_str(), policy.mode) == 0;
			if (!created && errno != EEXIST) {
				return log_slots_errno_failure("create runtime directory", path, errno);
			}
			runtime_internal::UniqueFd fd(
			    open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
			if (fd.get() < 0) {
				return log_slots_errno_failure("open runtime directory", path, errno);
			}
			if (created && (fchown(fd.get(), owner_uid, owner_gid) != 0 ||
			                fchmod(fd.get(), policy.mode) != 0)) {
				return false;
			}
			struct stat stat_{};
			if (fstat(fd.get(), &stat_) != 0 || !S_ISDIR(stat_.st_mode) ||
			    stat_.st_uid != owner_uid || stat_.st_gid != owner_gid ||
			    (stat_.st_mode & 07777) != policy.mode) {
				std::cerr << "Runtime directory failed strict validation: " << path << "\n";
				return false;
			}
			return true;
		}

	}  // namespace internal
}  // namespace howdy::native::auth_helper
