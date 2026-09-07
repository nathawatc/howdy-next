#include "internal.hpp"
#include "protocol/auth_helper_protocol.hpp"
#include "storage/staged_runtime_policy.hpp"
#include "support/atomic_files.hpp"
#include "support/user_names.hpp"

#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>

#include <sys/acl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <acl/libacl.h>

namespace howdy::native {

	namespace {

		using auth_helper_protocol::RuntimeGenerationSlot;
		using user_model_readiness_internal::StagedPathKind;
		using user_model_readiness_internal::StagedReadiness;

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

		auto ExpectedOwner(std::optional<uid_t> owner_uid) -> uid_t {
			return owner_uid.value_or(static_cast<uid_t>(0));
		}

		auto StatIsDirectory(const struct stat &stat, uid_t owner_uid, mode_t mode) -> bool {
			return S_ISDIR(stat.st_mode) && stat.st_uid == owner_uid && stat.st_gid == 0 &&
			       (stat.st_mode & 07777) == mode;
		}

		auto StatIsRegular(const struct stat &stat, uid_t owner_uid, mode_t mode, nlink_t links)
		    -> bool {
			return S_ISREG(stat.st_mode) && stat.st_uid == owner_uid && stat.st_gid == 0 &&
			       (stat.st_mode & 07777) == mode && stat.st_nlink == links;
		}

		auto AclMatches(int fd, const std::string &expected_text) -> bool {
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

		auto AclPermissionsText(StagedAclPermissions permissions) -> std::string {
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

		auto VerifyStagedAcl(StagedAclSubject subject, const StagedAclPolicy &policy) -> bool {
			std::string expected = "u::" + AclPermissionsText(policy.owner);
			if (policy.has_named_target) {
				expected += ",u:" + std::to_string(subject.target_uid) + ":" +
				            AclPermissionsText(policy.target);
			}
			expected += ",g::" + AclPermissionsText(policy.group);
			if (policy.has_named_target) {
				expected += ",m::" + AclPermissionsText(policy.mask);
			}
			expected += ",o::" + AclPermissionsText(policy.other);
			return AclMatches(subject.fd, expected);
		}

		auto ParseStagedPath(const std::filesystem::path &path, StagedPath *parsed)
		    -> StagedPathKind {
			if (!auth_helper_protocol::IsCanonicalAbsolutePath(path)) {
				const auto runtime_root = auth_helper_protocol::PreparedRuntimeRoot().string();
				const auto text         = path.string();
				if (text.starts_with(runtime_root + "/") || text == runtime_root) {
					return StagedPathKind::kMalformed;
				}
				for (const auto &component : path) {
					uid_t                 ignored_uid = 0;
					RuntimeGenerationSlot ignored_slot{};
					if (auth_helper_protocol::ParseRuntimeGenerationName(
					        component.string(), &ignored_uid, &ignored_slot)) {
						return StagedPathKind::kMalformed;
					}
				}
				return StagedPathKind::kCanonical;
			}

			const auto models_dir     = path.parent_path();
			const auto generation_dir = models_dir.parent_path();
			if (models_dir.filename() != auth_helper_protocol::kPreparedUserModelsDirectoryName ||
			    generation_dir.parent_path() != auth_helper_protocol::PreparedRuntimeRoot()) {
				const auto runtime_prefix =
				    auth_helper_protocol::PreparedRuntimeRoot().string() + "/";
				return path.string().starts_with(runtime_prefix) ? StagedPathKind::kMalformed
				                                                 : StagedPathKind::kCanonical;
			}

			uid_t                 target_uid = 0;
			RuntimeGenerationSlot slot{};
			const auto            generation_name = generation_dir.filename().string();
			if (!auth_helper_protocol::ParseRuntimeGenerationName(generation_name, &target_uid,
			                                                      &slot) ||
			    target_uid != getuid() ||
			    generation_dir !=
			        auth_helper_protocol::PreparedRuntimeGenerationDir(
			            auth_helper_protocol::PreparedRuntimeRoot(), target_uid, slot)) {
				return StagedPathKind::kMalformed;
			}

			const auto                 model_name = path.filename().string();
			constexpr std::string_view extension  = ".dat";
			if (!model_name.ends_with(extension)) {
				return StagedPathKind::kMalformed;
			}
			const auto user = model_name.substr(0, model_name.size() - extension.size());
			if (!IsValidModelUserName(user)) {
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

		auto StagedOpenFailure(int error_number) -> StagedReadiness {
			return error_number == EACCES || error_number == EIO ? StagedReadiness::kError
			                                                     : StagedReadiness::kInsecure;
		}

		auto OpenStagedDirectories(const StagedPath &path, uid_t owner_uid,
		                           StagedReadiness *failure) -> std::optional<StagedDirectories> {
			StagedDirectories directories{
			    .root = ScopedFd(open(auth_helper_protocol::kPreparedRuntimeRoot,
			                          O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)),
			};
			if (directories.root.Get() < 0) {
				*failure = StagedOpenFailure(errno);
				return std::nullopt;
			}
			struct stat root_stat{};
			if (fstat(directories.root.Get(), &root_stat) != 0) {
				*failure = StagedOpenFailure(errno);
				return std::nullopt;
			}
			if (!StatIsDirectory(root_stat, owner_uid,
			                     GetStagedRuntimePolicy(StagedRuntimeRole::kRuntimeRoot).mode)) {
				*failure = StagedReadiness::kInsecure;
				return std::nullopt;
			}

			directories.generation =
			    ScopedFd(openat(directories.root.Get(), path.generation_name.c_str(),
			                    O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
			if (directories.generation.Get() < 0) {
				*failure = StagedOpenFailure(errno);
				return std::nullopt;
			}
			struct stat generation_stat{};
			if (fstat(directories.generation.Get(), &generation_stat) != 0) {
				*failure = StagedOpenFailure(errno);
				return std::nullopt;
			}
			if (!StatIsDirectory(
			        generation_stat, owner_uid,
			        GetStagedRuntimePolicy(StagedRuntimeRole::kSharedDirectory).mode) ||
			    !VerifyStagedAcl(
			        {.fd = directories.generation.Get(), .target_uid = path.target_uid},
			        GetStagedRuntimePolicy(StagedRuntimeRole::kSharedDirectory).acl)) {
				*failure = StagedReadiness::kInsecure;
				return std::nullopt;
			}

			directories.models =
			    ScopedFd(openat(directories.generation.Get(),
			                    auth_helper_protocol::kPreparedUserModelsDirectoryName,
			                    O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
			if (directories.models.Get() < 0) {
				*failure = StagedOpenFailure(errno);
				return std::nullopt;
			}
			struct stat models_stat{};
			if (fstat(directories.models.Get(), &models_stat) != 0) {
				*failure = StagedOpenFailure(errno);
				return std::nullopt;
			}
			if (!StatIsDirectory(
			        models_stat, owner_uid,
			        GetStagedRuntimePolicy(StagedRuntimeRole::kSharedDirectory).mode) ||
			    !VerifyStagedAcl({.fd = directories.models.Get(), .target_uid = path.target_uid},
			                     GetStagedRuntimePolicy(StagedRuntimeRole::kSharedDirectory).acl)) {
				*failure = StagedReadiness::kInsecure;
				return std::nullopt;
			}
			return directories;
		}

		auto ValidatePresentStagedFile(int fd, const StagedPath &path,
		                               const StagedDirectories &directories, uid_t owner_uid)
		    -> bool {
			struct stat opened_stat{};
			struct stat visible_stat{};
			if (fd < 0 || fstat(fd, &opened_stat) != 0 ||
			    !StatIsRegular(opened_stat, owner_uid,
			                   GetStagedRuntimePolicy(StagedRuntimeRole::kPresentModel).mode,
			                   GetStagedRuntimePolicy(StagedRuntimeRole::kPresentModel)
			                       .exact_link_count.value_or(0)) ||
			    !VerifyStagedAcl({.fd = fd, .target_uid = path.target_uid},
			                     GetStagedRuntimePolicy(StagedRuntimeRole::kPresentModel).acl) ||
			    fstatat(directories.models.Get(), path.model_name.c_str(), &visible_stat,
			            AT_SYMLINK_NOFOLLOW) != 0 ||
			    !StatIsRegular(visible_stat, owner_uid,
			                   GetStagedRuntimePolicy(StagedRuntimeRole::kPresentModel).mode,
			                   GetStagedRuntimePolicy(StagedRuntimeRole::kPresentModel)
			                       .exact_link_count.value_or(0)) ||
			    visible_stat.st_dev != opened_stat.st_dev ||
			    visible_stat.st_ino != opened_stat.st_ino) {
				return false;
			}

			ScopedFd    backing(openat(directories.generation.Get(),
			                           auth_helper_protocol::kPreparedModelBackingFileName,
			                           O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
			struct stat backing_stat{};
			return backing.Get() >= 0 && fstat(backing.Get(), &backing_stat) == 0 &&
			       StatIsRegular(backing_stat, owner_uid,
			                     GetStagedRuntimePolicy(StagedRuntimeRole::kPresentModel).mode,
			                     GetStagedRuntimePolicy(StagedRuntimeRole::kPresentModel)
			                         .exact_link_count.value_or(0)) &&
			       backing_stat.st_dev == opened_stat.st_dev &&
			       backing_stat.st_ino == opened_stat.st_ino &&
			       VerifyStagedAcl({.fd = backing.Get(), .target_uid = path.target_uid},
			                       GetStagedRuntimePolicy(StagedRuntimeRole::kPresentModel).acl);
		}

	}  // namespace

	namespace user_model_readiness_internal {

		auto ClassifyStagedPath(const std::filesystem::path &path) -> StagedPathKind {
			return ParseStagedPath(path, nullptr);
		}

		auto InspectStagedModel(const std::filesystem::path &path, std::optional<uid_t> owner_uid)
		    -> StagedReadiness {
			StagedPath parsed;
			if (ParseStagedPath(path, &parsed) != StagedPathKind::kStaged) {
				return StagedReadiness::kInsecure;
			}
			const auto      owner       = ExpectedOwner(owner_uid);
			StagedReadiness failure     = StagedReadiness::kInsecure;
			auto            directories = OpenStagedDirectories(parsed, owner, &failure);
			if (!directories.has_value()) {
				return failure;
			}

			ScopedFd visible(openat(directories->models.Get(), parsed.model_name.c_str(),
			                        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
			if (visible.Get() >= 0) {
				return ValidatePresentStagedFile(visible.Get(), parsed, *directories, owner)
				           ? StagedReadiness::kPresent
				           : StagedReadiness::kInsecure;
			}
			if (errno != ENOENT) {
				return errno == EACCES || errno == EIO ? StagedReadiness::kError
				                                       : StagedReadiness::kInsecure;
			}

			ScopedFd    backing(openat(directories->generation.Get(),
			                           auth_helper_protocol::kPreparedModelBackingFileName,
			                           O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
			struct stat backing_stat{};
			if (backing.Get() < 0 || fstat(backing.Get(), &backing_stat) != 0) {
				return StagedReadiness::kInsecure;
			}
			const auto policy = GetStagedRuntimePolicy(StagedRuntimeRole::kAbsentModel);
			return StatIsRegular(backing_stat, owner, policy.mode,
			                     policy.exact_link_count.value_or(0)) &&
			               VerifyStagedAcl({.fd = backing.Get(), .target_uid = parsed.target_uid},
			                               policy.acl)
			           ? StagedReadiness::kAbsent
			           : StagedReadiness::kInsecure;
		}

		auto ValidateStagedModelFile(int fd, const std::filesystem::path &path,
		                             std::optional<uid_t> owner_uid) -> bool {
			StagedPath parsed;
			if (ParseStagedPath(path, &parsed) != StagedPathKind::kStaged) {
				return false;
			}
			const auto      owner       = ExpectedOwner(owner_uid);
			StagedReadiness failure     = StagedReadiness::kInsecure;
			auto            directories = OpenStagedDirectories(parsed, owner, &failure);
			return directories.has_value() &&
			       ValidatePresentStagedFile(fd, parsed, *directories, owner);
		}

	}  // namespace user_model_readiness_internal

}  // namespace howdy::native
