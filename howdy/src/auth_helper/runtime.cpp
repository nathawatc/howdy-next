#include "auth_helper/acl.hpp"
#include "auth_helper/runtime_internal.hpp"
#include "config/config_utils.hpp"
#include "config/runtime_paths.hpp"
#include "protocol/auth_helper_protocol.hpp"
#include "storage/staged_runtime_policy.hpp"
#include "storage/user_model_readiness.hpp"
#include "support/fd_io.hpp"
#include "support/user_names.hpp"

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
		constexpr std::size_t kCopyBufferSize = std::size_t{64} * 1024;

		using auth_helper_protocol::RuntimeGenerationSlot;
		using internal::RuntimeSources;
		using internal::StagedIdentity;

		class UniqueFd {
		public:
			UniqueFd() = default;

			explicit UniqueFd(int fd)
			    : fd_(fd) {}

			~UniqueFd() {
				reset();
			}

			UniqueFd(const UniqueFd &)                     = delete;
			auto operator=(const UniqueFd &) -> UniqueFd & = delete;

			UniqueFd(UniqueFd &&other) noexcept
			    : fd_(std::exchange(other.fd_, -1)) {}

			auto operator=(UniqueFd &&other) noexcept -> UniqueFd & {
				if (this != &other) {
					reset(std::exchange(other.fd_, -1));
				}
				return *this;
			}

			[[nodiscard]] auto get() const -> int {
				return fd_;
			}

			[[nodiscard]] auto release() -> int {
				return std::exchange(fd_, -1);
			}

			void reset(int fd = -1) {
				if (fd_ >= 0) {
					(void)close(fd_);
				}
				fd_ = fd;
			}

		private:
			int fd_ = -1;
		};

		struct SourceFile {
			UniqueFd    fd;
			struct stat initial_stat{};
		};

		struct Slot {
			std::filesystem::path path;
			UniqueFd              lock_fd;
			UniqueFd              dir_fd;
		};

		using SlotSet = std::array<std::optional<Slot>, 2>;

		auto log_errno_failure(std::string_view operation, const std::filesystem::path &path,
		                       int error_number) -> bool {
			std::cerr << "Failed to " << operation << " '" << path
			          << "': " << std::strerror(error_number) << "\n";
			return false;
		}

		auto stat_unchanged(const struct stat &before, const struct stat &after) -> bool {
			return before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
			       before.st_size == after.st_size &&
			       before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
			       before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
			       before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
			       before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
		}

		auto source_unchanged(const SourceFile &source) -> bool {
			struct stat current{};
			return fstat(source.fd.get(), &current) == 0 &&
			       stat_unchanged(source.initial_stat, current);
		}

		auto seek_start(int fd) -> bool {
			return lseek(fd, 0, SEEK_SET) == 0;
		}

		auto compare_files(int left_fd, int right_fd) -> bool {
			if (!seek_start(left_fd) || !seek_start(right_fd)) {
				return false;
			}
			std::array<char, kCopyBufferSize> left{};
			std::array<char, kCopyBufferSize> right{};
			while (true) {
				ssize_t left_size;
				do {
					left_size = read(left_fd, left.data(), left.size());
				} while (left_size < 0 && errno == EINTR);
				if (left_size < 0) {
					return false;
				}
				ssize_t right_size;
				do {
					right_size = read(right_fd, right.data(), right.size());
				} while (right_size < 0 && errno == EINTR);
				if (right_size < 0 || left_size != right_size) {
					return false;
				}
				if (left_size == 0) {
					return true;
				}
				if (std::memcmp(left.data(), right.data(), static_cast<std::size_t>(left_size)) !=
				    0) {
					return false;
				}
			}
		}

		auto copy_source_to_open_file(const SourceFile &source, int destination_fd) -> bool {
			if (!seek_start(source.fd.get()) || ftruncate(destination_fd, 0) != 0 ||
			    !seek_start(destination_fd)) {
				return false;
			}
			std::array<char, kCopyBufferSize> buffer{};
			while (true) {
				ssize_t size;
				do {
					size = read(source.fd.get(), buffer.data(), buffer.size());
				} while (size < 0 && errno == EINTR);
				if (size < 0) {
					return false;
				}
				if (size == 0) {
					break;
				}
				if (!howdy::native::write_all_to_fd(destination_fd, buffer.data(),
				                                    static_cast<std::size_t>(size))) {
					return false;
				}
			}
			return howdy::native::sync_fd(destination_fd) && source_unchanged(source);
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

		auto open_source_file(const std::filesystem::path &path, const std::string &label,
		                      uid_t owner_uid) -> std::optional<SourceFile> {
			UniqueFd fd(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
			if (fd.get() < 0) {
				log_errno_failure("open " + label, path, errno);
				return std::nullopt;
			}
			if (!internal::secure_source_file_stat(fd.get(), label, owner_uid)) {
				return std::nullopt;
			}
			SourceFile source{.fd = std::move(fd)};
			if (fstat(source.fd.get(), &source.initial_stat) != 0) {
				return std::nullopt;
			}
			return source;
		}

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
				log_errno_failure("open runtime lock", display_path, errno);
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

		auto open_slot_directory(int root_fd, const std::filesystem::path &root,
		                         RuntimeGenerationSlot generation, StagedIdentity identity,
		                         const AclOperations &operations, bool create)
		    -> std::optional<UniqueFd> {
			const auto name = auth_helper_protocol::prepared_runtime_generation_name(
			    identity.target_uid, generation);
			bool created = false;
			if (create && mkdirat(root_fd, name.c_str(), 0700) == 0) {
				created = true;
			} else if (create && errno != EEXIST) {
				return std::nullopt;
			}
			UniqueFd fd(
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
		                           bool create) -> std::optional<UniqueFd> {
			bool created = false;
			if (create && mkdirat(slot_fd, auth_helper_protocol::kPreparedUserModelsDirectoryName,
			                      0700) == 0) {
				created = true;
			} else if (create && errno != EEXIST) {
				return std::nullopt;
			}
			UniqueFd fd(openat(slot_fd, auth_helper_protocol::kPreparedUserModelsDirectoryName,
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
		    -> std::optional<UniqueFd> {
			bool created = false;
			int  raw_fd  = openat(slot_fd, auth_helper_protocol::kPreparedConfigFileName,
			                      O_RDWR | O_CLOEXEC | O_NOFOLLOW);
			if (create && raw_fd < 0 && errno == ENOENT) {
				raw_fd  = openat(slot_fd, auth_helper_protocol::kPreparedConfigFileName,
				                 O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
				created = raw_fd >= 0;
			}
			UniqueFd fd(raw_fd);
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
		                        bool create) -> std::optional<UniqueFd> {
			bool created = false;
			int  raw_fd  = openat(slot_fd, auth_helper_protocol::kPreparedModelBackingFileName,
			                      O_RDWR | O_CLOEXEC | O_NOFOLLOW);
			if (create && raw_fd < 0 && errno == ENOENT) {
				raw_fd  = openat(slot_fd, auth_helper_protocol::kPreparedModelBackingFileName,
				                 O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
				created = raw_fd >= 0;
			}
			UniqueFd fd(raw_fd);
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
			UniqueFd duplicate(openat(models_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
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

		auto slot_is_fresh(Slot &slot, const SourceFile &config_source,
		                   const std::optional<SourceFile> &model_source, const std::string &user,
		                   StagedIdentity identity, const AclOperations &operations) -> bool {
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
			    !compare_files(config_source.fd.get(), config->get()) ||
			    !source_unchanged(config_source)) {
				return false;
			}
			if (!model_source.has_value()) {
				return model_state == ModelState::kAbsent;
			}
			return model_state == ModelState::kPresent &&
			       compare_files(model_source->fd.get(), backing->get()) &&
			       source_unchanged(*model_source);
		}

		auto update_slot(Slot &slot, const SourceFile &config_source,
		                 const std::optional<SourceFile> &model_source, const std::string &user,
		                 StagedIdentity identity, const AclOperations &operations) -> bool {
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
			if (!copy_source_to_open_file(config_source, config->get()) ||
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
				if (!copy_source_to_open_file(*model_source, backing->get()) ||
				    !apply_private_acl(backing->get(), visible_path, identity,
				                       StagedRuntimeRole::kPresentModel, operations)) {
					std::cerr << "Failed to update persistent model in " << slot.path << "\n";
					return false;
				}
				if (state == ModelState::kAbsent &&
				    linkat(slot.dir_fd.get(), auth_helper_protocol::kPreparedModelBackingFileName,
				           models->get(), model_name.c_str(), 0) != 0) {
					return log_errno_failure("link visible model", visible_path, errno);
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
			return valid && source_unchanged(config_source) &&
			       (!model_source.has_value() || source_unchanged(*model_source));
		}

		auto open_slot(int root_fd, const std::filesystem::path &root,
		               RuntimeGenerationSlot generation, StagedIdentity identity,
		               const AclOperations &operations, bool create) -> std::optional<Slot> {
			const auto lock_name = auth_helper_protocol::prepared_runtime_generation_lock_name(
			    identity.target_uid, generation);
			auto lock = open_root_only_lock(root_fd, lock_name, root / lock_name, identity,
			                                operations, create);
			if (!lock.has_value()) {
				return std::nullopt;
			}
			auto directory =
			    open_slot_directory(root_fd, root, generation, identity, operations, create);
			if (!directory.has_value()) {
				return std::nullopt;
			}
			return Slot{.path = auth_helper_protocol::prepared_runtime_generation_dir(
			                root, identity.target_uid, generation),
			            .lock_fd = std::move(*lock),
			            .dir_fd  = std::move(*directory)};
		}

		auto prepared_paths(Slot &slot) -> std::optional<PreparedPaths> {
			const auto  lock_path = std::filesystem::path(slot.path.string() + ".lock");
			UniqueFd    lease(open(lock_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
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

		auto remove_tree_contents(int directory_fd) -> bool {
			UniqueFd duplicate(dup(directory_fd));
			if (duplicate.get() < 0) {
				return false;
			}
			DIR *directory = fdopendir(duplicate.release());
			if (directory == nullptr) {
				return false;
			}
			bool ok = true;
			while (ok) {
				errno         = 0;
				dirent *entry = readdir(directory);
				if (entry == nullptr) {
					ok = errno == 0;
					break;
				}
				const std::string_view name(entry->d_name);
				if (name == "." || name == "..") {
					continue;
				}
				struct stat stat_{};
				if (fstatat(directory_fd, entry->d_name, &stat_, AT_SYMLINK_NOFOLLOW) != 0) {
					ok = false;
					break;
				}
				if (S_ISDIR(stat_.st_mode)) {
					UniqueFd child(openat(directory_fd, entry->d_name,
					                      O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
					if (child.get() < 0 || fchmod(child.get(), 0700) != 0 ||
					    !remove_tree_contents(child.get()) ||
					    unlinkat(directory_fd, entry->d_name, AT_REMOVEDIR) != 0) {
						ok = false;
					}
				} else if (unlinkat(directory_fd, entry->d_name, 0) != 0) {
					ok = false;
				}
			}
			return closedir(directory) == 0 && ok;
		}

	}  // namespace

	namespace internal {
		auto validate_runtime_root(const std::filesystem::path &path, uid_t owner_uid,
		                           gid_t owner_gid) -> bool {
			const auto policy  = staged_runtime_policy(StagedRuntimeRole::kRuntimeRoot);
			const bool created = mkdir(path.c_str(), policy.mode) == 0;
			if (!created && errno != EEXIST) {
				return log_errno_failure("create runtime directory", path, errno);
			}
			UniqueFd fd(open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
			if (fd.get() < 0) {
				return log_errno_failure("open runtime directory", path, errno);
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

		auto secure_source_file_stat(int fd, const std::string &label, uid_t owner_uid) -> bool {
			struct stat stat_{};
			if (fstat(fd, &stat_) != 0) {
				std::cerr << "Failed to inspect " << label << ": " << std::strerror(errno) << "\n";
				return false;
			}
			if (!S_ISREG(stat_.st_mode) || stat_.st_uid != owner_uid ||
			    (stat_.st_mode & (S_IWGRP | S_IWOTH)) != 0 || stat_.st_nlink != 1) {
				std::cerr << label << " failed secure file validation\n";
				return false;
			}
			return true;
		}

		auto select_source_model_path(const std::filesystem::path &source_user_models_dir,
		                              const std::string &user, std::optional<uid_t> owner_uid,
		                              std::optional<std::filesystem::path> &source_model_path)
		    -> bool {
			source_model_path.reset();
			const auto readiness =
			    howdy::native::check_user_model_readiness(source_user_models_dir, user, owner_uid);
			switch (readiness.status) {
				case howdy::native::UserModelStatus::kOk:
					source_model_path = readiness.path;
					return true;
				case howdy::native::UserModelStatus::kNoModel:
				case howdy::native::UserModelStatus::kNoModelDirectory:
					return true;
				case howdy::native::UserModelStatus::kInvalidUser:
					std::cerr << howdy::native::kInvalidUserNameMessage << "\n";
					return false;
				default:
					std::cerr << (readiness.error_message.empty()
					                  ? "Failed to validate user model file"
					                  : readiness.error_message)
					          << "\n";
					return false;
			}
		}

		auto prepare_runtime_auth_files(const std::string &user, StagedIdentity identity,
		                                const RuntimeSources &sources,
		                                const AclOperations  &operations)
		    -> std::optional<PreparedPaths> {
			auto root =
			    open_or_create_root(sources.runtime_root, identity.owner_uid, identity.owner_gid);
			if (!root.has_value()) {
				return std::nullopt;
			}
			const auto allocation_name = ".pam-" + std::to_string(identity.target_uid) + ".lock";
			auto allocation = open_root_only_lock(root->get(), allocation_name,
			                                      sources.runtime_root / allocation_name, identity,
			                                      operations, true);
			if (!allocation.has_value()) {
				return std::nullopt;
			}
			while (flock(allocation->get(), LOCK_EX) != 0) {
				if (errno != EINTR) {
					return std::nullopt;
				}
			}

			auto config_source = open_source_file(sources.config, howdy::native::kConfigFileLabel,
			                                      identity.owner_uid);
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
			std::optional<SourceFile> model_source;
			if (model_path.has_value()) {
				model_source = open_source_file(*model_path, std::string(kUserModelFileLabel),
				                                identity.owner_uid);
				if (!model_source.has_value()) {
					return std::nullopt;
				}
			}

			auto slots = open_slots(root->get(), sources, identity, operations);
			if (!slots.has_value()) {
				return std::nullopt;
			}
			if (auto fresh = lease_fresh_slot(*slots, *config_source, model_source, user, identity,
			                                  operations);
			    fresh.has_value()) {
				return fresh;
			}
			return refresh_available_slot(*slots, *config_source, model_source, user, identity,
			                              operations);
		}

		auto cleanup_runtime_auth_files(const std::filesystem::path &path, uid_t uid,
		                                const std::filesystem::path &runtime_root, uid_t owner_uid,
		                                gid_t owner_gid) -> CleanupRuntimeResult {
			if (!auth_helper_protocol::is_canonical_absolute_path(path) ||
			    !auth_helper_protocol::is_canonical_absolute_path(runtime_root) ||
			    path.parent_path() != runtime_root) {
				return {.ok            = false,
				        .error_message = "Refusing to clean unexpected runtime directory"};
			}
			auto root = open_or_create_root(runtime_root, owner_uid, owner_gid);
			if (!root.has_value()) {
				return {.ok = false, .error_message = "Refusing to clean insecure runtime root"};
			}

			uid_t                 parsed_uid = 0;
			RuntimeGenerationSlot generation{};
			if (auth_helper_protocol::parse_runtime_generation_name(path.filename().string(),
			                                                        &parsed_uid, &generation)) {
				if (parsed_uid != uid) {
					return {.ok            = false,
					        .error_message = "Refusing to clean another user's runtime directory"};
				}
				auto slot =
				    open_slot(root->get(), runtime_root, generation,
				              {.target_uid = uid, .owner_uid = owner_uid, .owner_gid = owner_gid},
				              production_acl_operations(), false);
				return slot.has_value()
				           ? CleanupRuntimeResult{.ok = true}
				           : CleanupRuntimeResult{.ok = false,
				                                  .error_message =
				                                      "Refusing to clean malformed runtime slot"};
			}

			if (!auth_helper_protocol::matches_legacy_runtime_directory_name(
			        path.filename().string(), uid)) {
				return {.ok            = false,
				        .error_message = "Refusing to clean unexpected runtime directory"};
			}
			UniqueFd directory(openat(root->get(), path.filename().c_str(),
			                          O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
			if (directory.get() < 0) {
				return errno == ENOENT
				           ? CleanupRuntimeResult{.ok = true}
				           : CleanupRuntimeResult{.ok = false,
				                                  .error_message =
				                                      "Failed to inspect runtime directory"};
			}
			struct stat stat_{};
			if (fstat(directory.get(), &stat_) != 0 || !S_ISDIR(stat_.st_mode) ||
			    stat_.st_uid != owner_uid || stat_.st_gid != owner_gid ||
			    (stat_.st_mode & (S_IWGRP | S_IWOTH)) != 0 || fchmod(directory.get(), 0700) != 0 ||
			    !remove_tree_contents(directory.get()) ||
			    unlinkat(root->get(), path.filename().c_str(), AT_REMOVEDIR) != 0) {
				return {.ok = false, .error_message = "Failed to clean runtime directory"};
			}
			return {.ok = true};
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

	auto cleanup_runtime_auth_files(const std::filesystem::path &path, RuntimeIdentity identity)
	    -> CleanupRuntimeResult {
		(void)identity.gid;
		return internal::cleanup_runtime_auth_files(path, identity.uid, runtime_root(), 0, 0);
	}

}  // namespace howdy::native::auth_helper
