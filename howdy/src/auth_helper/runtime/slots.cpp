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

		auto LogSlotsErrnoFailure(std::string_view operation, const std::filesystem::path &path,
		                          int error_number) -> bool {
			std::cerr << "Failed to " << operation << " '" << path
			          << "': " << std::strerror(error_number) << "\n";
			return false;
		}

		auto ValidateRegular(const struct stat &stat, StagedIdentity identity,
		                     StagedRuntimeRole role) -> bool {
			const auto policy = GetStagedRuntimePolicy(role);
			return policy.exact_link_count.has_value() && S_ISREG(stat.st_mode) &&
			       stat.st_uid == identity.owner_uid && stat.st_gid == identity.owner_gid &&
			       (stat.st_mode & 07777) == policy.mode &&
			       stat.st_nlink == *policy.exact_link_count;
		}

		auto ValidateDirectory(const struct stat &stat, StagedIdentity identity,
		                       StagedRuntimeRole role) -> bool {
			const auto policy = GetStagedRuntimePolicy(role);
			return S_ISDIR(stat.st_mode) && stat.st_uid == identity.owner_uid &&
			       stat.st_gid == identity.owner_gid && (stat.st_mode & 07777) == policy.mode;
		}

		auto SetOwner(int fd, StagedIdentity identity) -> bool {
			struct stat stat{};
			if (fstat(fd, &stat) != 0) {
				return false;
			}
			return (stat.st_uid == identity.owner_uid && stat.st_gid == identity.owner_gid) ||
			       fchown(fd, identity.owner_uid, identity.owner_gid) == 0;
		}

		auto ApplyPrivateAcl(int fd, const std::filesystem::path &path, StagedIdentity identity,
		                     StagedRuntimeRole role, const AclOperations &operations) -> bool {
			const auto policy = GetStagedRuntimePolicy(role);
			return SetOwner(fd, identity) && fchmod(fd, 0600) == 0 &&
			       SetPersistentAclWithOperations(fd, path, identity.target_uid, policy.acl,
			                                      operations) &&
			       fchmod(fd, policy.mode) == 0 &&
			       VerifyPersistentAclWithOperations(fd, path, identity.target_uid, policy.acl,
			                                         operations);
		}

		auto ApplyOwnerOnlyAcl(int fd, const std::filesystem::path &path, StagedRuntimeRole role,
		                       const AclOperations &operations) -> bool {
			const auto policy = GetStagedRuntimePolicy(role);
			return fchmod(fd, policy.mode) == 0 &&
			       SetPersistentAclWithOperations(fd, path, uid_t{0}, policy.acl, operations) &&
			       fchmod(fd, policy.mode) == 0 &&
			       VerifyPersistentAclWithOperations(fd, path, uid_t{0}, policy.acl, operations);
		}

		auto OpenSlotDirectory(int root_fd, const std::filesystem::path &root,
		                       RuntimeGenerationSlot generation, StagedIdentity identity,
		                       const AclOperations &operations, bool create)
		    -> std::optional<runtime_internal::UniqueFd> {
			const auto name = auth_helper_protocol::PreparedRuntimeGenerationName(
			    identity.target_uid, generation);
			bool created = false;
			if (create && mkdirat(root_fd, name.c_str(), 0700) == 0) {
				created = true;
			} else if (create && errno != EEXIST) {
				return std::nullopt;
			}
			runtime_internal::UniqueFd fd(
			    openat(root_fd, name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
			if (fd.Get() < 0) {
				return std::nullopt;
			}
			const auto path = root / name;
			if (created && !ApplyPrivateAcl(fd.Get(), path, identity,
			                                StagedRuntimeRole::kSharedDirectory, operations)) {
				return std::nullopt;
			}
			struct stat stat{};
			if (fstat(fd.Get(), &stat) != 0 ||
			    !ValidateDirectory(stat, identity, StagedRuntimeRole::kSharedDirectory) ||
			    !VerifyPersistentAclWithOperations(
			        fd.Get(), path, identity.target_uid,
			        GetStagedRuntimePolicy(StagedRuntimeRole::kSharedDirectory).acl, operations)) {
				std::cerr << "Runtime slot failed strict validation: " << path << "\n";
				return std::nullopt;
			}
			return fd;
		}

		auto OpenModelsDirectory(int slot_fd, const std::filesystem::path &slot_path,
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
			if (fd.Get() < 0) {
				return std::nullopt;
			}
			const auto path = auth_helper_protocol::PreparedUserModelsDir(slot_path);
			if (created && !ApplyPrivateAcl(fd.Get(), path, identity,
			                                StagedRuntimeRole::kSharedDirectory, operations)) {
				return std::nullopt;
			}
			struct stat stat{};
			if (fstat(fd.Get(), &stat) != 0 ||
			    !ValidateDirectory(stat, identity, StagedRuntimeRole::kSharedDirectory) ||
			    !VerifyPersistentAclWithOperations(
			        fd.Get(), path, identity.target_uid,
			        GetStagedRuntimePolicy(StagedRuntimeRole::kSharedDirectory).acl, operations)) {
				return std::nullopt;
			}
			return fd;
		}

		auto OpenConfigFile(int slot_fd, const std::filesystem::path &slot_path,
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
			if (fd.Get() < 0) {
				return std::nullopt;
			}
			const auto path = auth_helper_protocol::PreparedConfigPath(slot_path);
			if (created && !ApplyPrivateAcl(fd.Get(), path, identity, StagedRuntimeRole::kConfig,
			                                operations)) {
				return std::nullopt;
			}
			struct stat stat{};
			if (fstat(fd.Get(), &stat) != 0 ||
			    !ValidateRegular(stat, identity, StagedRuntimeRole::kConfig) ||
			    !VerifyPersistentAclWithOperations(
			        fd.Get(), path, identity.target_uid,
			        GetStagedRuntimePolicy(StagedRuntimeRole::kConfig).acl, operations)) {
				return std::nullopt;
			}
			return fd;
		}

		auto OpenModelBacking(int slot_fd, const std::filesystem::path &slot_path,
		                      StagedIdentity identity, const AclOperations &operations, bool create)
		    -> std::optional<runtime_internal::UniqueFd> {
			bool created = false;
			int  raw_fd  = openat(slot_fd, auth_helper_protocol::kPreparedModelBackingFileName,
			                      O_RDWR | O_CLOEXEC | O_NOFOLLOW);
			if (create && raw_fd < 0 && errno == ENOENT) {
				raw_fd  = openat(slot_fd, auth_helper_protocol::kPreparedModelBackingFileName,
				                 O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
				created = raw_fd >= 0;
			}
			runtime_internal::UniqueFd fd(raw_fd);
			if (fd.Get() < 0) {
				return std::nullopt;
			}
			const auto path = slot_path / auth_helper_protocol::kPreparedModelBackingFileName;
			if (created &&
			    (!SetOwner(fd.Get(), identity) ||
			     !ApplyOwnerOnlyAcl(fd.Get(), path, StagedRuntimeRole::kAbsentModel, operations))) {
				return std::nullopt;
			}
			return fd;
		}

		auto ModelsDirectoryContainsOnly(int models_fd, std::string_view expected_name,
		                                 bool expected_present) -> bool {
			runtime_internal::UniqueFd duplicate(
			    openat(models_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
			if (duplicate.Get() < 0) {
				return false;
			}
			DIR *directory = fdopendir(duplicate.Release());
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

		auto SlotInitialization(int slot_fd) -> SlotInitialization {
			std::array<bool, 3>  present{};
			constexpr std::array names = {auth_helper_protocol::kPreparedConfigFileName,
			                              auth_helper_protocol::kPreparedUserModelsDirectoryName,
			                              auth_helper_protocol::kPreparedModelBackingFileName};
			for (std::size_t index = 0; index < names.size(); ++index) {
				struct stat stat{};
				if (fstatat(slot_fd, names[index], &stat, AT_SYMLINK_NOFOLLOW) == 0) {
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

		auto InspectModelState(int models_fd, const std::filesystem::path &slot_path,
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
			    auth_helper_protocol::PreparedUserModelsDir(slot_path) / (user + ".dat");
			struct stat visible_stat{};
			const bool  visible = fstatat(models_fd, (user + ".dat").c_str(), &visible_stat,
			                              AT_SYMLINK_NOFOLLOW) == 0;
			if (!visible && errno != ENOENT) {
				return ModelState::kInvalid;
			}
			if (!visible) {
				const auto policy = GetStagedRuntimePolicy(StagedRuntimeRole::kAbsentModel);
				return ValidateRegular(backing_stat, identity, StagedRuntimeRole::kAbsentModel) &&
				               VerifyPersistentAclWithOperations(backing_fd, backing_path, uid_t{0},
				                                                 policy.acl, operations) &&
				               ModelsDirectoryContainsOnly(models_fd, user + ".dat", false)
				           ? ModelState::kAbsent
				           : ModelState::kInvalid;
			}
			return ValidateRegular(backing_stat, identity, StagedRuntimeRole::kPresentModel) &&
			               ValidateRegular(visible_stat, identity,
			                               StagedRuntimeRole::kPresentModel) &&
			               backing_stat.st_dev == visible_stat.st_dev &&
			               backing_stat.st_ino == visible_stat.st_ino &&
			               VerifyPersistentAclWithOperations(
			                   backing_fd, model_path, identity.target_uid,
			                   GetStagedRuntimePolicy(StagedRuntimeRole::kPresentModel).acl,
			                   operations) &&
			               ModelsDirectoryContainsOnly(models_fd, user + ".dat", true)
			           ? ModelState::kPresent
			           : ModelState::kInvalid;
		}

		auto SlotIsFresh(runtime_internal::Slot                            &slot,
		                 const runtime_internal::SourceFile                &config_source,
		                 const std::optional<runtime_internal::SourceFile> &model_source,
		                 const std::string &user, StagedIdentity identity,
		                 const AclOperations &operations) -> bool {
			auto models =
			    OpenModelsDirectory(slot.dir_fd.Get(), slot.path, identity, operations, false);
			auto config = OpenConfigFile(slot.dir_fd.Get(), slot.path, identity, operations, false);
			auto backing =
			    OpenModelBacking(slot.dir_fd.Get(), slot.path, identity, operations, false);
			if (!models.has_value() || !config.has_value() || !backing.has_value()) {
				return false;
			}
			const auto model_state = InspectModelState(models->Get(), slot.path, user,
			                                           backing->Get(), identity, operations);
			if (model_state == ModelState::kInvalid ||
			    !runtime_internal::CompareFiles(config_source.fd.Get(), config->Get()) ||
			    !runtime_internal::SourceUnchanged(config_source)) {
				return false;
			}
			if (!model_source.has_value()) {
				return model_state == ModelState::kAbsent;
			}
			return model_state == ModelState::kPresent &&
			       runtime_internal::CompareFiles(model_source->fd.Get(), backing->Get()) &&
			       runtime_internal::SourceUnchanged(*model_source);
		}

		auto UpdateSlot(runtime_internal::Slot                            &slot,
		                const runtime_internal::SourceFile                &config_source,
		                const std::optional<runtime_internal::SourceFile> &model_source,
		                const std::string &user, StagedIdentity identity,
		                const AclOperations &operations) -> bool {
			const auto initialization = SlotInitialization(slot.dir_fd.Get());
			if (initialization == SlotInitialization::kInvalid) {
				std::cerr << "Persistent runtime objects are incomplete in " << slot.path << "\n";
				return false;
			}
			const bool create = initialization == SlotInitialization::kEmpty;
			auto       models =
			    OpenModelsDirectory(slot.dir_fd.Get(), slot.path, identity, operations, create);
			auto config =
			    OpenConfigFile(slot.dir_fd.Get(), slot.path, identity, operations, create);
			auto backing =
			    OpenModelBacking(slot.dir_fd.Get(), slot.path, identity, operations, create);
			if (!models.has_value() || !config.has_value() || !backing.has_value()) {
				std::cerr << "Failed to open persistent runtime objects in " << slot.path
				          << " (models=" << models.has_value() << ", config=" << config.has_value()
				          << ", backing=" << backing.has_value() << ")\n";
				return false;
			}
			const auto state = InspectModelState(models->Get(), slot.path, user, backing->Get(),
			                                     identity, operations);
			if (state == ModelState::kInvalid) {
				std::cerr << "Persistent model state failed strict validation in " << slot.path
				          << "\n";
				return false;
			}
			if (!runtime_internal::CopySourceToOpenFile(config_source, config->Get()) ||
			    !ApplyPrivateAcl(config->Get(), auth_helper_protocol::PreparedConfigPath(slot.path),
			                     identity, StagedRuntimeRole::kConfig, operations)) {
				std::cerr << "Failed to update persistent config in " << slot.path << "\n";
				return false;
			}

			const std::string model_name = user + ".dat";
			const auto        backing_path =
			    slot.path / auth_helper_protocol::kPreparedModelBackingFileName;
			const auto visible_path =
			    auth_helper_protocol::PreparedUserModelsDir(slot.path) / model_name;
			if (model_source.has_value()) {
				if (!runtime_internal::CopySourceToOpenFile(*model_source, backing->Get()) ||
				    !ApplyPrivateAcl(backing->Get(), visible_path, identity,
				                     StagedRuntimeRole::kPresentModel, operations)) {
					std::cerr << "Failed to update persistent model in " << slot.path << "\n";
					return false;
				}
				if (state == ModelState::kAbsent &&
				    linkat(slot.dir_fd.Get(), auth_helper_protocol::kPreparedModelBackingFileName,
				           models->Get(), model_name.c_str(), 0) != 0) {
					return LogSlotsErrnoFailure("link visible model", visible_path, errno);
				}
			} else {
				if (state == ModelState::kPresent &&
				    unlinkat(models->Get(), model_name.c_str(), 0) != 0) {
					return false;
				}
				if (ftruncate(backing->Get(), 0) != 0 || !howdy::native::SyncFd(backing->Get()) ||
				    !ApplyOwnerOnlyAcl(backing->Get(), backing_path,
				                       StagedRuntimeRole::kAbsentModel, operations)) {
					return false;
				}
			}
			if (!howdy::native::SyncFd(models->Get()) ||
			    !howdy::native::SyncFd(slot.dir_fd.Get())) {
				std::cerr << "Failed to sync persistent runtime directories in " << slot.path
				          << "\n";
				return false;
			}
			const bool valid = InspectModelState(models->Get(), slot.path, user, backing->Get(),
			                                     identity, operations) != ModelState::kInvalid;
			if (!valid) {
				std::cerr << "Updated model state failed strict validation in " << slot.path
				          << "\n";
			}
			return valid && runtime_internal::SourceUnchanged(config_source) &&
			       (!model_source.has_value() || runtime_internal::SourceUnchanged(*model_source));
		}

		auto OpenSlot(int root_fd, const std::filesystem::path &root,
		              RuntimeGenerationSlot generation, StagedIdentity identity,
		              const AclOperations &operations, bool create)
		    -> std::optional<runtime_internal::Slot> {
			const auto lock_name = auth_helper_protocol::PreparedRuntimeGenerationLockName(
			    identity.target_uid, generation);
			auto lock = runtime_internal::OpenRootOnlyLock(root_fd, lock_name, root / lock_name,
			                                               identity, operations, create);
			if (!lock.has_value()) {
				return std::nullopt;
			}
			auto directory =
			    OpenSlotDirectory(root_fd, root, generation, identity, operations, create);
			if (!directory.has_value()) {
				return std::nullopt;
			}
			return runtime_internal::Slot{.path =
			                                  auth_helper_protocol::PreparedRuntimeGenerationDir(
			                                      root, identity.target_uid, generation),
			                              .lock_fd = std::move(*lock),
			                              .dir_fd  = std::move(*directory)};
		}

		auto BuildPreparedPaths(runtime_internal::Slot &slot) -> std::optional<PreparedPaths> {
			const auto lock_path = std::filesystem::path(slot.path.string() + ".lock");
			runtime_internal::UniqueFd lease(
			    open(lock_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
			struct stat writable_stat{};
			struct stat lease_stat{};
			const auto  policy = GetStagedRuntimePolicy(StagedRuntimeRole::kLock);
			if (lease.Get() < 0 || fstat(slot.lock_fd.Get(), &writable_stat) != 0 ||
			    fstat(lease.Get(), &lease_stat) != 0 || writable_stat.st_dev != lease_stat.st_dev ||
			    writable_stat.st_ino != lease_stat.st_ino || !S_ISREG(lease_stat.st_mode) ||
			    (lease_stat.st_mode & 07777) != policy.mode ||
			    !policy.exact_link_count.has_value() ||
			    lease_stat.st_nlink != *policy.exact_link_count ||
			    flock(lease.Get(), LOCK_SH | LOCK_NB) != 0) {
				return std::nullopt;
			}
			return PreparedPaths{.runtime_dir = slot.path,
			                     .config_path = auth_helper_protocol::PreparedConfigPath(slot.path),
			                     .user_models_dir =
			                         auth_helper_protocol::PreparedUserModelsDir(slot.path),
			                     .lease_fd = lease.Release()};
		}
	}  // namespace

	namespace runtime_internal {

		auto OpenOrCreateRoot(const std::filesystem::path &path, uid_t owner_uid, gid_t owner_gid)
		    -> std::optional<UniqueFd> {
			if (!internal::ValidateRuntimeRoot(path, owner_uid, owner_gid)) {
				return std::nullopt;
			}
			UniqueFd fd(open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
			if (fd.Get() < 0) {
				return std::nullopt;
			}
			struct stat stat{};
			const auto  policy = GetStagedRuntimePolicy(StagedRuntimeRole::kRuntimeRoot);
			if (fstat(fd.Get(), &stat) != 0 || !S_ISDIR(stat.st_mode) || stat.st_uid != owner_uid ||
			    stat.st_gid != owner_gid || (stat.st_mode & 07777) != policy.mode) {
				return std::nullopt;
			}
			return fd;
		}

		auto OpenRootOnlyLock(int root_fd, const std::string &name,
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
			if (fd.Get() < 0) {
				LogSlotsErrnoFailure("open runtime lock", display_path, errno);
				return std::nullopt;
			}
			if (created && (!SetOwner(fd.Get(), identity) ||
			                !ApplyOwnerOnlyAcl(fd.Get(), display_path, StagedRuntimeRole::kLock,
			                                   operations))) {
				return std::nullopt;
			}
			struct stat stat{};
			const auto  policy = GetStagedRuntimePolicy(StagedRuntimeRole::kLock);
			if (fstat(fd.Get(), &stat) != 0 ||
			    !ValidateRegular(stat, identity, StagedRuntimeRole::kLock) ||
			    !VerifyPersistentAclWithOperations(fd.Get(), display_path, uid_t{0}, policy.acl,
			                                       operations)) {
				std::cerr << "Runtime lock failed strict validation: " << display_path << "\n";
				return std::nullopt;
			}
			return fd;
		}

		auto OpenSlots(int root_fd, const RuntimeSources &sources, StagedIdentity identity,
		               const AclOperations &operations) -> std::optional<SlotSet> {
			constexpr std::array generations = {RuntimeGenerationSlot::kSlot0,
			                                    RuntimeGenerationSlot::kSlot1};
			SlotSet              slots;
			bool                 any_valid = false;
			for (std::size_t index = 0; index < slots.size(); ++index) {
				slots[index] = OpenSlot(root_fd, sources.runtime_root, generations[index], identity,
				                        operations, true);
				any_valid    = any_valid || slots[index].has_value();
			}
			return any_valid ? std::optional<SlotSet>{std::move(slots)} : std::nullopt;
		}

		auto LeaseFreshSlot(SlotSet &slots, const SourceFile &config_source,
		                    const std::optional<SourceFile> &model_source, const std::string &user,
		                    StagedIdentity identity, const AclOperations &operations)
		    -> std::optional<PreparedPaths> {
			for (auto &slot : slots) {
				if (!slot.has_value() || flock(slot->lock_fd.Get(), LOCK_SH | LOCK_NB) != 0) {
					continue;
				}
				if (SlotIsFresh(*slot, config_source, model_source, user, identity, operations)) {
					auto prepared = BuildPreparedPaths(*slot);
					if (prepared.has_value()) {
						return prepared;
					}
				}
				(void)flock(slot->lock_fd.Get(), LOCK_UN);
			}
			return std::nullopt;
		}

		auto RefreshAvailableSlot(SlotSet &slots, const SourceFile &config_source,
		                          const std::optional<SourceFile> &model_source,
		                          const std::string &user, StagedIdentity identity,
		                          const AclOperations &operations) -> std::optional<PreparedPaths> {
			for (auto &slot : slots) {
				if (!slot.has_value() || flock(slot->lock_fd.Get(), LOCK_EX | LOCK_NB) != 0) {
					continue;
				}
				if (!UpdateSlot(*slot, config_source, model_source, user, identity, operations)) {
					slot->lock_fd.Reset();
					continue;
				}
				if (flock(slot->lock_fd.Get(), LOCK_SH | LOCK_NB) != 0) {
					slot->lock_fd.Reset();
					continue;
				}
				auto prepared = BuildPreparedPaths(*slot);
				if (prepared.has_value()) {
					return prepared;
				}
			}
			std::cerr << "No usable auth-helper runtime slot\n";
			return std::nullopt;
		}

	}  // namespace runtime_internal

	namespace internal {

		auto ValidateRuntimeRoot(const std::filesystem::path &path, uid_t owner_uid,
		                         gid_t owner_gid) -> bool {
			const auto policy  = GetStagedRuntimePolicy(StagedRuntimeRole::kRuntimeRoot);
			const bool created = mkdir(path.c_str(), policy.mode) == 0;
			if (!created && errno != EEXIST) {
				return LogSlotsErrnoFailure("create runtime directory", path, errno);
			}
			runtime_internal::UniqueFd fd(
			    open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
			if (fd.Get() < 0) {
				return LogSlotsErrnoFailure("open runtime directory", path, errno);
			}
			if (created && (fchown(fd.Get(), owner_uid, owner_gid) != 0 ||
			                fchmod(fd.Get(), policy.mode) != 0)) {
				return false;
			}
			struct stat stat{};
			if (fstat(fd.Get(), &stat) != 0 || !S_ISDIR(stat.st_mode) || stat.st_uid != owner_uid ||
			    stat.st_gid != owner_gid || (stat.st_mode & 07777) != policy.mode) {
				std::cerr << "Runtime directory failed strict validation: " << path << "\n";
				return false;
			}
			return true;
		}

	}  // namespace internal
}  // namespace howdy::native::auth_helper
