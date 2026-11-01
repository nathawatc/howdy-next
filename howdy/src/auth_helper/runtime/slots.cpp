#include "auth_helper/acl.hpp"
#include "auth_helper/runtime.hpp"
#include "auth_helper/runtime/internal.hpp"
#include "internal.hpp"
#include "protocol/auth_helper_protocol.hpp"
#include "storage/staged_runtime_policy.hpp"
#include "support/bit_flags.hpp"
#include "support/eintr.hpp"
#include "support/error_message.hpp"
#include "support/fd_io.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#include <linux/fs.h>

namespace howdy::native::auth_helper {
	namespace {
		using auth_helper_protocol::RuntimeGenerationSlot;
		using internal::RuntimeSources;
		using internal::StagedIdentity;

		auto LogSlotsErrnoFailure(std::string_view operation, const std::filesystem::path &path,
		                          int error_number) -> bool {
			std::cerr << "Failed to " << operation << " '" << path
			          << "': " << howdy::native::SystemErrorMessage(error_number) << "\n";
			return false;
		}

		auto ValidateRegular(const struct stat &stat, StagedIdentity identity,
		                     StagedRuntimeRole role) -> bool {
			const auto policy = GetStagedRuntimePolicy(role);
			return policy.exact_link_count.has_value() && S_ISREG(stat.st_mode) &&
			       stat.st_uid == identity.owner_uid && stat.st_gid == identity.owner_gid &&
			       MaskedBits(stat.st_mode, 07777) == policy.mode &&
			       stat.st_nlink == *policy.exact_link_count;
		}

		auto ValidateDirectory(const struct stat &stat, StagedIdentity identity,
		                       StagedRuntimeRole role) -> bool {
			const auto policy = GetStagedRuntimePolicy(role);
			return S_ISDIR(stat.st_mode) && stat.st_uid == identity.owner_uid &&
			       stat.st_gid == identity.owner_gid &&
			       MaskedBits(stat.st_mode, 07777) == policy.mode;
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

		auto ValidSlotDirectory(int fd, const std::filesystem::path &path, StagedIdentity identity,
		                        const AclOperations &operations) -> bool {
			struct stat stat{};
			if (fstat(fd, &stat) != 0 ||
			    !ValidateDirectory(stat, identity, StagedRuntimeRole::kSharedDirectory) ||
			    !VerifyPersistentAclWithOperations(
			        fd, path, identity.target_uid,
			        GetStagedRuntimePolicy(StagedRuntimeRole::kSharedDirectory).acl, operations)) {
				std::cerr << "Runtime slot failed strict validation: " << path << "\n";
				return false;
			}
			return true;
		}

		// Creates a new slot directory, used only for side directories being built.
		auto CreateSlotDirectory(int root_fd, const std::string &name,
		                         const std::filesystem::path &path, StagedIdentity identity,
		                         const AclOperations &operations)
		    -> std::optional<runtime_internal::UniqueFd> {
			if (mkdirat(root_fd, name.c_str(), 0700) != 0) {
				const int error_number = errno;
				LogSlotsErrnoFailure("create runtime slot", path, error_number);
				return std::nullopt;
			}
			runtime_internal::UniqueFd fd(
			    openat(root_fd, name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
			if (!fd.Valid() ||
			    !ApplyPrivateAcl(fd.Get(), path, identity, StagedRuntimeRole::kSharedDirectory,
			                     operations) ||
			    !ValidSlotDirectory(fd.Get(), path, identity, operations)) {
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

		struct DirectoryCloser {
			void operator()(DIR *directory) const {
				(void)closedir(directory);
			}
		};

		// Names in a directory other than "." and "..", or nullopt when it cannot be read.
		auto DirectoryEntries(int directory_fd) -> std::optional<std::vector<std::string>> {
			runtime_internal::UniqueFd duplicate(
			    openat(directory_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
			if (duplicate.Get() < 0) {
				return std::nullopt;
			}
			const std::unique_ptr<DIR, DirectoryCloser> directory(fdopendir(duplicate.Get()));
			if (directory == nullptr) {
				return std::nullopt;
			}
			(void)duplicate.Release();
			std::vector<std::string> names;
			while (true) {
				errno               = 0;
				const dirent *entry = readdir(directory.get());
				if (entry == nullptr) {
					break;
				}
				const auto             name_bytes = std::span{entry->d_name};
				const auto             name_end   = std::ranges::find(name_bytes, '\0');
				const std::string_view name(
				    name_bytes.data(), static_cast<std::size_t>(name_end - name_bytes.begin()));
				if (name != "." && name != "..") {
					names.emplace_back(name);
				}
			}
			if (errno != 0) {
				return std::nullopt;
			}
			return names;
		}

		auto ModelsDirectoryContainsOnly(int models_fd, std::string_view expected_name,
		                                 bool expected_present) -> bool {
			const auto names = DirectoryEntries(models_fd);
			if (!names.has_value()) {
				return false;
			}
			return expected_present ? names->size() == 1 && names->front() == expected_name
			                        : names->empty();
		}

		enum class ModelState : std::uint8_t {
			kAbsent,
			kPresent,
			kInvalid,
		};

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
			if (!slot.dir_fd.Valid()) {
				return false;
			}
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

		enum class SlotWrite : std::uint8_t {
			kWritten,
			kUnusable,  // the slot's objects are missing or fail strict validation
			kFailed,    // the write itself failed, for example because a source changed
		};

		// Writes the sources into the slot's fixed objects: in place for an existing slot, or into
		// freshly created objects when `create` is set for a side directory being built.
		auto WriteSlotContents(const runtime_internal::Slot                      &slot,
		                       const runtime_internal::SourceFile                &config_source,
		                       const std::optional<runtime_internal::SourceFile> &model_source,
		                       const std::string &user, StagedIdentity identity,
		                       const AclOperations &operations, bool create) -> SlotWrite {
			auto models =
			    OpenModelsDirectory(slot.dir_fd.Get(), slot.path, identity, operations, create);
			auto config =
			    OpenConfigFile(slot.dir_fd.Get(), slot.path, identity, operations, create);
			auto backing =
			    OpenModelBacking(slot.dir_fd.Get(), slot.path, identity, operations, create);
			if (!models.has_value() || !config.has_value() || !backing.has_value()) {
				std::cerr << "Failed to open persistent runtime objects in " << slot.path
				          << " (models=" << models.has_value() << ", config=" << config.has_value()
				          << ", backing=" << backing.has_value() << ")\n";
				return SlotWrite::kUnusable;
			}
			const auto state = InspectModelState(models->Get(), slot.path, user, backing->Get(),
			                                     identity, operations);
			if (state == ModelState::kInvalid) {
				std::cerr << "Persistent model state failed strict validation in " << slot.path
				          << "\n";
				return SlotWrite::kUnusable;
			}
			if (!runtime_internal::CopySourceToOpenFile(config_source, config->Get()) ||
			    !ApplyPrivateAcl(config->Get(), auth_helper_protocol::PreparedConfigPath(slot.path),
			                     identity, StagedRuntimeRole::kConfig, operations)) {
				std::cerr << "Failed to update persistent config in " << slot.path << "\n";
				return SlotWrite::kFailed;
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
					return SlotWrite::kFailed;
				}
				if (state == ModelState::kAbsent &&
				    linkat(slot.dir_fd.Get(), auth_helper_protocol::kPreparedModelBackingFileName,
				           models->Get(), model_name.c_str(), 0) != 0) {
					LogSlotsErrnoFailure("link visible model", visible_path, errno);
					return SlotWrite::kFailed;
				}
			} else {
				if (state == ModelState::kPresent &&
				    unlinkat(models->Get(), model_name.c_str(), 0) != 0) {
					LogSlotsErrnoFailure("unlink visible model", visible_path, errno);
					return SlotWrite::kFailed;
				}
				if (ftruncate(backing->Get(), 0) != 0 || !howdy::native::SyncFd(backing->Get()) ||
				    !ApplyOwnerOnlyAcl(backing->Get(), backing_path,
				                       StagedRuntimeRole::kAbsentModel, operations)) {
					std::cerr << "Failed to clear persistent model in " << slot.path << "\n";
					return SlotWrite::kFailed;
				}
			}
			if (!howdy::native::SyncFd(models->Get()) ||
			    !howdy::native::SyncFd(slot.dir_fd.Get())) {
				std::cerr << "Failed to sync persistent runtime directories in " << slot.path
				          << "\n";
				return SlotWrite::kFailed;
			}
			if (InspectModelState(models->Get(), slot.path, user, backing->Get(), identity,
			                      operations) == ModelState::kInvalid) {
				std::cerr << "Updated model state failed strict validation in " << slot.path
				          << "\n";
				return SlotWrite::kUnusable;
			}
			const bool sources_unchanged =
			    runtime_internal::SourceUnchanged(config_source) &&
			    (!model_source.has_value() || runtime_internal::SourceUnchanged(*model_source));
			return sources_unchanged ? SlotWrite::kWritten : SlotWrite::kFailed;
		}

		// Deepest nesting removed from a stale side directory; a slot itself nests one level.
		constexpr int kMaxStaleTreeDepth = 4;

		// Removes `name` under `parent_fd` with everything below it. Used only for side
		// directories in the runtime root, which only root can write, so their contents are known
		// to be ours. unlinkat and O_NOFOLLOW never follow a symlink out of the root. Recursion is
		// bounded by kMaxStaleTreeDepth; anything nested deeper fails closed and stays in place.
		auto RemoveStaleTree(int parent_fd, const std::string &name,  // NOLINT(misc-no-recursion)
		                     const std::filesystem::path &path, int depth_left) -> bool {
			const runtime_internal::UniqueFd directory(
			    openat(parent_fd, name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
			if (!directory.Valid()) {
				const int error_number = errno;
				if (error_number == ENOENT) {
					return true;
				}
				// A non-directory (or symlink) at the name is a leftover too; unlink the name.
				if ((error_number == ENOTDIR || error_number == ELOOP) &&
				    unlinkat(parent_fd, name.c_str(), 0) == 0) {
					return true;
				}
				return LogSlotsErrnoFailure("open stale runtime directory", path, error_number);
			}
			const auto names = DirectoryEntries(directory.Get());
			if (!names.has_value()) {
				std::cerr << "Failed to list stale runtime directory " << path << "\n";
				return false;
			}
			for (const auto &entry : *names) {
				if (unlinkat(directory.Get(), entry.c_str(), 0) == 0) {
					continue;
				}
				const int error_number = errno;
				if (error_number != EISDIR || depth_left == 0) {
					return LogSlotsErrnoFailure("remove stale runtime object", path / entry,
					                            error_number);
				}
				if (!RemoveStaleTree(directory.Get(), entry, path / entry, depth_left - 1)) {
					return false;
				}
			}
			if (unlinkat(parent_fd, name.c_str(), AT_REMOVEDIR) != 0) {
				const int error_number = errno;
				return LogSlotsErrnoFailure("remove stale runtime directory", path, error_number);
			}
			return true;
		}

		auto InstallSlotDirectory(int root_fd, const std::string &from, const std::string &to,
		                          bool replacing) -> int {
#if defined(SYS_renameat2) && defined(RENAME_EXCHANGE) && defined(RENAME_NOREPLACE)
			if (RetryOnEintr([&] -> decltype(syscall(0)) {
				    return syscall(SYS_renameat2, root_fd, from.c_str(), root_fd, to.c_str(),
					               replacing ? RENAME_EXCHANGE : RENAME_NOREPLACE);
			    }) == 0) {
				return 0;
			}
#else
			errno = ENOSYS;
#endif
			// Without renameat2 a new slot is still installed with renameat: the allocation lock
			// guarantees nothing exists at the final name. Replacing a slot needs the exchange.
			if (!replacing && (errno == ENOSYS || errno == EINVAL)) {
				return renameat(root_fd, from.c_str(), root_fd, to.c_str());
			}
			return -1;
		}

		// Builds a complete slot in a side directory and swaps it into place, so a kill at any
		// point leaves either the previous slot directory or the finished one, never a half-built
		// slot. Runs under the slot's exclusive lock; the side directory name is fixed per slot,
		// so leftovers of an interrupted build are removed first. Rebuilding a slot that was only
		// briefly unreadable is harmless: it gets a fresh copy of the trusted sources.
		auto RebuildSlot(int root_fd, runtime_internal::Slot &slot,
		                 const runtime_internal::SourceFile                &config_source,
		                 const std::optional<runtime_internal::SourceFile> &model_source,
		                 const std::string &user, StagedIdentity identity,
		                 const AclOperations &operations) -> bool {
			const auto             final_name = slot.path.filename().string();
			runtime_internal::Slot build{
			    .path = auth_helper_protocol::PreparedRuntimeGenerationBuildPath(slot.path),
			};
			const auto build_name = build.path.filename().string();
			const bool replacing  = slot.dir_fd.Valid();
			if (replacing) {
				std::cerr << "Rebuilding runtime slot that could not be opened or validated: "
				          << slot.path << "\n";
			}
			// OpenSlot already removed any leftover under the allocation lock; if one remains,
			// mkdirat fails with EEXIST and the build fails closed.
			auto directory =
			    CreateSlotDirectory(root_fd, build_name, build.path, identity, operations);
			if (directory.has_value()) {
				build.dir_fd = std::move(*directory);
			}
			if (!build.dir_fd.Valid() ||
			    WriteSlotContents(build, config_source, model_source, user, identity, operations,
			                      true) != SlotWrite::kWritten) {
				build.dir_fd.Reset();
				(void)RemoveStaleTree(root_fd, build_name, build.path, kMaxStaleTreeDepth);
				return false;
			}
			if (InstallSlotDirectory(root_fd, build_name, final_name, replacing) != 0) {
				const int error_number = errno;
				LogSlotsErrnoFailure("install rebuilt runtime slot", slot.path, error_number);
				build.dir_fd.Reset();
				(void)RemoveStaleTree(root_fd, build_name, build.path, kMaxStaleTreeDepth);
				return false;
			}
			// The rename committed the new slot, so later failures here only leave cleanup behind:
			// the previous directory now at the side name is removed again before the next build.
			slot.dir_fd = std::move(build.dir_fd);
			if (replacing) {
				(void)RemoveStaleTree(root_fd, build_name, build.path, kMaxStaleTreeDepth);
			}
			if (!howdy::native::SyncFd(root_fd)) {
				const int error_number = errno;
				LogSlotsErrnoFailure("sync runtime directory", slot.path.parent_path(),
				                     error_number);
			}
			return true;
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
			runtime_internal::Slot slot{
			    .path = auth_helper_protocol::PreparedRuntimeGenerationDir(
			        root, identity.target_uid, generation),
			    .lock_fd = std::move(*lock),
			};
			// Every builder holds the allocation lock, so a side directory seen here was left by an
			// interrupted run: remove it before it can linger with stale staged copies.
			const auto side_path =
			    auth_helper_protocol::PreparedRuntimeGenerationBuildPath(slot.path);
			(void)RemoveStaleTree(root_fd, side_path.filename().string(), side_path,
			                      kMaxStaleTreeDepth);
			// A missing slot directory is built by the first refresh; an existing one must pass
			// strict validation and is never replaced if it does not.
			runtime_internal::UniqueFd directory(
			    openat(root_fd, slot.path.filename().c_str(),
				       O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
			if (!directory.Valid()) {
				return errno == ENOENT ? std::optional{std::move(slot)} : std::nullopt;
			}
			if (!ValidSlotDirectory(directory.Get(), slot.path, identity, operations)) {
				return std::nullopt;
			}
			slot.dir_fd = std::move(directory);
			return slot;
		}

		auto BuildPreparedPaths(runtime_internal::Slot &slot) -> std::optional<PreparedPaths> {
			const auto lock_path =
			    auth_helper_protocol::PreparedRuntimeGenerationLockPath(slot.path);
			runtime_internal::UniqueFd lease(
			    open(lock_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
			struct stat writable_stat{};
			struct stat lease_stat{};
			const auto  policy = GetStagedRuntimePolicy(StagedRuntimeRole::kLock);
			if (!lease.Valid() || fstat(slot.lock_fd.Get(), &writable_stat) != 0 ||
			    fstat(lease.Get(), &lease_stat) != 0 || writable_stat.st_dev != lease_stat.st_dev ||
			    writable_stat.st_ino != lease_stat.st_ino || !S_ISREG(lease_stat.st_mode) ||
			    MaskedBits(lease_stat.st_mode, 07777) != policy.mode ||
			    !policy.exact_link_count.has_value() ||
			    lease_stat.st_nlink != *policy.exact_link_count ||
			    flock(lease.Get(), LOCK_SH | LOCK_NB) != 0) {
				return std::nullopt;
			}
			return PreparedPaths{
			    .runtime_dir     = slot.path,
			    .config_path     = auth_helper_protocol::PreparedConfigPath(slot.path),
			    .user_models_dir = auth_helper_protocol::PreparedUserModelsDir(slot.path),
			    .lease_fd        = std::move(lease),
			};
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
			    stat.st_gid != owner_gid || MaskedBits(stat.st_mode, 07777) != policy.mode) {
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
			constexpr std::array generations = {
			    RuntimeGenerationSlot::kSlot0,
			    RuntimeGenerationSlot::kSlot1,
			};
			SlotSet slots;
			bool    any_valid = false;
			static_assert(generations.size() == slots.size());
			for (auto &&[slot, generation] : std::views::zip(slots, generations)) {
				slot =
				    OpenSlot(root_fd, sources.runtime_root, generation, identity, operations, true);
				any_valid = any_valid || slot.has_value();
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

		auto RefreshAvailableSlot(int root_fd, SlotSet &slots, const SourceFile &config_source,
		                          const std::optional<SourceFile> &model_source,
		                          const std::string &user, StagedIdentity identity,
		                          const AclOperations &operations) -> std::optional<PreparedPaths> {
			for (auto &slot : slots) {
				if (!slot.has_value() || flock(slot->lock_fd.Get(), LOCK_EX | LOCK_NB) != 0) {
					continue;
				}
				// A healthy slot is updated in place and keeps its inodes; a missing slot, or one
				// whose objects fail validation, is rebuilt and swapped in. A failed write leaves
				// the slot for the next run to inspect, and is never answered with a rebuild.
				const auto written = slot->dir_fd.Valid()
				                         ? WriteSlotContents(*slot, config_source, model_source,
				                                             user, identity, operations, false)
				                         : SlotWrite::kUnusable;
				if (written == SlotWrite::kFailed ||
				    (written == SlotWrite::kUnusable &&
				     !RebuildSlot(root_fd, *slot, config_source, model_source, user, identity,
				                  operations))) {
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
			runtime_internal::UniqueFd const fd(
			    open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
			if (fd.Get() < 0) {
				return LogSlotsErrnoFailure("open runtime directory", path, errno);
			}
			// Staging creates the root with umask 0 and the owner's group, so this is normally a
			// no-op; it still corrects what a parent's setgid bit or default ACL imposes.
			if (created && (fchown(fd.Get(), owner_uid, owner_gid) != 0 ||
			                fchmod(fd.Get(), policy.mode) != 0)) {
				return false;
			}
			struct stat stat{};
			if (fstat(fd.Get(), &stat) != 0 || !S_ISDIR(stat.st_mode) || stat.st_uid != owner_uid ||
			    stat.st_gid != owner_gid || MaskedBits(stat.st_mode, 07777) != policy.mode) {
				std::cerr << "Runtime directory failed strict validation: " << path << "\n";
				return false;
			}
			return true;
		}

	}  // namespace internal
}  // namespace howdy::native::auth_helper
