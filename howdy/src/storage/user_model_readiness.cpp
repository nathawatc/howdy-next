#include "storage/user_model_readiness.hpp"

#include "protocol/auth_helper_protocol.hpp"
#include "storage/staged_runtime_policy.hpp"
#include "support/atomic_files.hpp"
#include "support/file_security.hpp"
#include "support/user_names.hpp"

#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>

#include <sys/acl.h>
#include <sys/stat.h>

#include <acl/libacl.h>

namespace howdy::native {

	namespace {

		using auth_helper_protocol::RuntimeGenerationSlot;

		enum class StagedPathKind : std::uint8_t {
			kCanonical,
			kMalformed,
			kStaged,
		};

		struct StagedPath {
			std::filesystem::path generation_dir;
			std::string           generation_name;
			std::string           model_name;
			uid_t                 target_uid = 0;
		};

		struct StagedDirectories {
			ScopedFd root;
			ScopedFd generation;
			ScopedFd models;
		};

		enum class StagedReadiness : std::uint8_t {
			kPresent,
			kAbsent,
			kInsecure,
			kError,
		};

		auto readiness_failure(UserModelStatus status, std::string message,
		                       std::filesystem::path path = {}) -> UserModelReadinessResult {
			return UserModelReadinessResult{
			    .status        = status,
			    .error_message = std::move(message),
			    .path          = std::move(path),
			};
		}

		auto expected_owner(std::optional<uid_t> owner_uid) -> uid_t {
			return owner_uid.value_or(static_cast<uid_t>(0));
		}

		auto stat_is_directory(const struct stat &stat_, uid_t owner_uid, mode_t mode) -> bool {
			return S_ISDIR(stat_.st_mode) && stat_.st_uid == owner_uid && stat_.st_gid == 0 &&
			       (stat_.st_mode & 07777) == mode;
		}

		auto stat_is_regular(const struct stat &stat_, uid_t owner_uid, mode_t mode, nlink_t links)
		    -> bool {
			return S_ISREG(stat_.st_mode) && stat_.st_uid == owner_uid && stat_.st_gid == 0 &&
			       (stat_.st_mode & 07777) == mode && stat_.st_nlink == links;
		}

		auto acl_matches(int fd, const std::string &expected_text) -> bool {
			acl_t      actual   = acl_get_fd(fd);
			acl_t      expected = acl_from_text(expected_text.c_str());
			const bool matches  = actual != nullptr && expected != nullptr &&
			                      acl_valid(expected) == 0 && acl_cmp(actual, expected) == 0;
			if (actual != nullptr) {
				acl_free(actual);
			}
			if (expected != nullptr) {
				acl_free(expected);
			}
			return matches;
		}

		auto acl_permissions_text(StagedAclPermissions permissions) -> std::string {
			std::string text = "---";
			text[0]          = permissions.read ? 'r' : '-';
			text[1]          = permissions.write ? 'w' : '-';
			text[2]          = permissions.execute ? 'x' : '-';
			return text;
		}

		struct StagedAclSubject {
			int   fd;
			uid_t target_uid;
		};

		auto verify_staged_acl(StagedAclSubject subject, const StagedAclPolicy &policy) -> bool {
			std::string expected = "u::" + acl_permissions_text(policy.owner);
			if (policy.has_named_target) {
				expected += ",u:" + std::to_string(subject.target_uid) + ":" +
				            acl_permissions_text(policy.target);
			}
			expected += ",g::" + acl_permissions_text(policy.group);
			if (policy.has_named_target) {
				expected += ",m::" + acl_permissions_text(policy.mask);
			}
			expected += ",o::" + acl_permissions_text(policy.other);
			return acl_matches(subject.fd, expected);
		}

		auto parse_staged_path(const std::filesystem::path &path, StagedPath *parsed)
		    -> StagedPathKind {
			if (!auth_helper_protocol::is_canonical_absolute_path(path)) {
				const auto runtime_root = auth_helper_protocol::prepared_runtime_root().string();
				const auto text         = path.string();
				if (text.starts_with(runtime_root + "/") || text == runtime_root) {
					return StagedPathKind::kMalformed;
				}
				for (const auto &component : path) {
					uid_t                 ignored_uid = 0;
					RuntimeGenerationSlot ignored_slot{};
					if (auth_helper_protocol::parse_runtime_generation_name(
					        component.string(), &ignored_uid, &ignored_slot)) {
						return StagedPathKind::kMalformed;
					}
				}
				return StagedPathKind::kCanonical;
			}

			const auto models_dir     = path.parent_path();
			const auto generation_dir = models_dir.parent_path();
			if (models_dir.filename() != auth_helper_protocol::kPreparedUserModelsDirectoryName ||
			    generation_dir.parent_path() != auth_helper_protocol::prepared_runtime_root()) {
				const auto runtime_prefix =
				    auth_helper_protocol::prepared_runtime_root().string() + "/";
				return path.string().starts_with(runtime_prefix) ? StagedPathKind::kMalformed
				                                                 : StagedPathKind::kCanonical;
			}

			uid_t                 target_uid = 0;
			RuntimeGenerationSlot slot{};
			const auto            generation_name = generation_dir.filename().string();
			if (!auth_helper_protocol::parse_runtime_generation_name(generation_name, &target_uid,
			                                                         &slot) ||
			    target_uid != getuid() ||
			    generation_dir !=
			        auth_helper_protocol::prepared_runtime_generation_dir(
			            auth_helper_protocol::prepared_runtime_root(), target_uid, slot)) {
				return StagedPathKind::kMalformed;
			}

			const auto                 model_name = path.filename().string();
			constexpr std::string_view extension  = ".dat";
			if (!model_name.ends_with(extension)) {
				return StagedPathKind::kMalformed;
			}
			const auto user = model_name.substr(0, model_name.size() - extension.size());
			if (!is_valid_model_user_name(user)) {
				return StagedPathKind::kMalformed;
			}

			if (parsed != nullptr) {
				*parsed = StagedPath{
				    .generation_dir  = generation_dir,
				    .generation_name = generation_name,
				    .model_name      = model_name,
				    .target_uid      = target_uid,
				};
			}
			return StagedPathKind::kStaged;
		}

		auto staged_open_failure(int error_number) -> StagedReadiness {
			return error_number == EACCES || error_number == EIO ? StagedReadiness::kError
			                                                     : StagedReadiness::kInsecure;
		}

		auto open_staged_directories(const StagedPath &path, uid_t owner_uid,
		                             StagedReadiness *failure) -> std::optional<StagedDirectories> {
			StagedDirectories directories{
			    .root = ScopedFd(open(auth_helper_protocol::kPreparedRuntimeRoot,
			                          O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)),
			};
			if (directories.root.get() < 0) {
				*failure = staged_open_failure(errno);
				return std::nullopt;
			}
			struct stat root_stat{};
			if (fstat(directories.root.get(), &root_stat) != 0) {
				*failure = staged_open_failure(errno);
				return std::nullopt;
			}
			if (!stat_is_directory(root_stat, owner_uid,
			                       staged_runtime_policy(StagedRuntimeRole::kRuntimeRoot).mode)) {
				*failure = StagedReadiness::kInsecure;
				return std::nullopt;
			}

			directories.generation =
			    ScopedFd(openat(directories.root.get(), path.generation_name.c_str(),
			                    O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
			if (directories.generation.get() < 0) {
				*failure = staged_open_failure(errno);
				return std::nullopt;
			}
			struct stat generation_stat{};
			if (fstat(directories.generation.get(), &generation_stat) != 0) {
				*failure = staged_open_failure(errno);
				return std::nullopt;
			}
			if (!stat_is_directory(
			        generation_stat, owner_uid,
			        staged_runtime_policy(StagedRuntimeRole::kSharedDirectory).mode) ||
			    !verify_staged_acl(
			        {.fd = directories.generation.get(), .target_uid = path.target_uid},
			        staged_runtime_policy(StagedRuntimeRole::kSharedDirectory).acl)) {
				*failure = StagedReadiness::kInsecure;
				return std::nullopt;
			}

			directories.models =
			    ScopedFd(openat(directories.generation.get(),
			                    auth_helper_protocol::kPreparedUserModelsDirectoryName,
			                    O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
			if (directories.models.get() < 0) {
				*failure = staged_open_failure(errno);
				return std::nullopt;
			}
			struct stat models_stat{};
			if (fstat(directories.models.get(), &models_stat) != 0) {
				*failure = staged_open_failure(errno);
				return std::nullopt;
			}
			if (!stat_is_directory(
			        models_stat, owner_uid,
			        staged_runtime_policy(StagedRuntimeRole::kSharedDirectory).mode) ||
			    !verify_staged_acl(
			        {.fd = directories.models.get(), .target_uid = path.target_uid},
			        staged_runtime_policy(StagedRuntimeRole::kSharedDirectory).acl)) {
				*failure = StagedReadiness::kInsecure;
				return std::nullopt;
			}
			return directories;
		}

		auto validate_present_staged_file(int fd, const StagedPath &path,
		                                  const StagedDirectories &directories, uid_t owner_uid)
		    -> bool {
			struct stat opened_stat{};
			struct stat visible_stat{};
			if (fd < 0 || fstat(fd, &opened_stat) != 0 ||
			    !stat_is_regular(opened_stat, owner_uid,
			                     staged_runtime_policy(StagedRuntimeRole::kPresentModel).mode,
			                     staged_runtime_policy(StagedRuntimeRole::kPresentModel)
			                         .exact_link_count.value_or(0)) ||
			    !verify_staged_acl({.fd = fd, .target_uid = path.target_uid},
			                       staged_runtime_policy(StagedRuntimeRole::kPresentModel).acl) ||
			    fstatat(directories.models.get(), path.model_name.c_str(), &visible_stat,
			            AT_SYMLINK_NOFOLLOW) != 0 ||
			    !stat_is_regular(visible_stat, owner_uid,
			                     staged_runtime_policy(StagedRuntimeRole::kPresentModel).mode,
			                     staged_runtime_policy(StagedRuntimeRole::kPresentModel)
			                         .exact_link_count.value_or(0)) ||
			    visible_stat.st_dev != opened_stat.st_dev ||
			    visible_stat.st_ino != opened_stat.st_ino) {
				return false;
			}

			ScopedFd    backing(openat(directories.generation.get(),
			                           auth_helper_protocol::kPreparedModelBackingFileName,
			                           O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
			struct stat backing_stat{};
			return backing.get() >= 0 && fstat(backing.get(), &backing_stat) == 0 &&
			       stat_is_regular(backing_stat, owner_uid,
			                       staged_runtime_policy(StagedRuntimeRole::kPresentModel).mode,
			                       staged_runtime_policy(StagedRuntimeRole::kPresentModel)
			                           .exact_link_count.value_or(0)) &&
			       backing_stat.st_dev == opened_stat.st_dev &&
			       backing_stat.st_ino == opened_stat.st_ino &&
			       verify_staged_acl({.fd = backing.get(), .target_uid = path.target_uid},
			                         staged_runtime_policy(StagedRuntimeRole::kPresentModel).acl);
		}

		auto inspect_staged_model(const std::filesystem::path &path, std::optional<uid_t> owner_uid)
		    -> StagedReadiness {
			StagedPath parsed;
			if (parse_staged_path(path, &parsed) != StagedPathKind::kStaged) {
				return StagedReadiness::kInsecure;
			}
			const auto      owner       = expected_owner(owner_uid);
			StagedReadiness failure     = StagedReadiness::kInsecure;
			auto            directories = open_staged_directories(parsed, owner, &failure);
			if (!directories.has_value()) {
				return failure;
			}

			ScopedFd visible(openat(directories->models.get(), parsed.model_name.c_str(),
			                        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
			if (visible.get() >= 0) {
				return validate_present_staged_file(visible.get(), parsed, *directories, owner)
				           ? StagedReadiness::kPresent
				           : StagedReadiness::kInsecure;
			}
			if (errno != ENOENT) {
				return errno == EACCES || errno == EIO ? StagedReadiness::kError
				                                       : StagedReadiness::kInsecure;
			}

			ScopedFd    backing(openat(directories->generation.get(),
			                           auth_helper_protocol::kPreparedModelBackingFileName,
			                           O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
			struct stat backing_stat{};
			if (backing.get() < 0 || fstat(backing.get(), &backing_stat) != 0) {
				return StagedReadiness::kInsecure;
			}
			const auto policy = staged_runtime_policy(StagedRuntimeRole::kAbsentModel);
			return stat_is_regular(backing_stat, owner, policy.mode,
			                       policy.exact_link_count.value_or(0)) &&
			               verify_staged_acl({.fd = backing.get(), .target_uid = parsed.target_uid},
			                                 policy.acl)
			           ? StagedReadiness::kAbsent
			           : StagedReadiness::kInsecure;
		}

	}  // namespace

	auto validate_staged_user_model_file(int fd, const std::filesystem::path &path,
	                                     std::optional<uid_t> owner_uid) -> bool {
		StagedPath parsed;
		if (parse_staged_path(path, &parsed) != StagedPathKind::kStaged) {
			return false;
		}
		const auto      owner       = expected_owner(owner_uid);
		StagedReadiness failure     = StagedReadiness::kInsecure;
		auto            directories = open_staged_directories(parsed, owner, &failure);
		return directories.has_value() &&
		       validate_present_staged_file(fd, parsed, *directories, owner);
	}

	auto check_user_model_readiness(const std::filesystem::path &models_dir,
	                                const std::string &user, std::optional<uid_t> owner_uid)
	    -> UserModelReadinessResult {
		const auto model_path = resolve_user_model_path(models_dir, user);
		if (!model_path) {
			return readiness_failure(UserModelStatus::kInvalidUser, kInvalidUserNameMessage);
		}

		const auto staged_kind = parse_staged_path(*model_path, nullptr);
		if (staged_kind != StagedPathKind::kCanonical) {
			if (staged_kind == StagedPathKind::kMalformed) {
				return readiness_failure(UserModelStatus::kInsecurePath,
				                         "Staged user model path failed validation: " +
				                             model_path->string(),
				                         *model_path);
			}
			switch (inspect_staged_model(*model_path, owner_uid)) {
				case StagedReadiness::kPresent:
					return UserModelReadinessResult{.status = UserModelStatus::kOk,
					                                .path   = *model_path};
				case StagedReadiness::kAbsent:
					return readiness_failure(UserModelStatus::kNoModel, {}, *model_path);
				case StagedReadiness::kError:
					return readiness_failure(UserModelStatus::kParseError,
					                         "Failed to inspect staged user model: " +
					                             model_path->string(),
					                         *model_path);
				case StagedReadiness::kInsecure:
					return readiness_failure(UserModelStatus::kInsecurePath,
					                         "Staged user model failed security validation: " +
					                             model_path->string(),
					                         *model_path);
			}
		}

		const auto directory_security = check_secure_root_owned_directory_tree(
		    models_dir, kUserModelsDirectoryLabel, owner_uid);
		if (!directory_security.ok) {
			if (directory_security.error_code == ENOENT) {
				return readiness_failure(UserModelStatus::kNoModelDirectory, {}, *model_path);
			}
			if (directory_security.error_code != 0) {
				return readiness_failure(UserModelStatus::kParseError,
				                         directory_security.error_message, *model_path);
			}
			return readiness_failure(UserModelStatus::kInsecurePath,
			                         directory_security.error_message, *model_path);
		}

		const auto file_security = check_secure_root_owned_file_with_directory(
		    *model_path, {.directory = kUserModelsDirectoryLabel, .file = kUserModelFileLabel},
		    owner_uid);
		if (!file_security.ok) {
			if (file_security.error_code == ENOENT) {
				return readiness_failure(UserModelStatus::kNoModel, {}, *model_path);
			}
			if (file_security.error_code != 0) {
				return readiness_failure(UserModelStatus::kParseError, file_security.error_message,
				                         *model_path);
			}
			return readiness_failure(UserModelStatus::kInsecurePath, file_security.error_message,
			                         *model_path);
		}

		return UserModelReadinessResult{
		    .status = UserModelStatus::kOk,
		    .path   = *model_path,
		};
	}

}  // namespace howdy::native
