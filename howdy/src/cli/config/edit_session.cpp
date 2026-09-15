#include "cli/config/edit_session.hpp"

#include "config/config_utils.hpp"
#include "config/runtime_paths.hpp"
#include "config/test_hooks.hpp"
#include "support/fd_io.hpp"
#include "support/invoking_user_env.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <grp.h>
#include <optional>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

#include <sys/stat.h>
#include <sys/wait.h>

namespace howdy::native::config_internal {

	namespace {

		namespace fs = std::filesystem;

		auto ValidationRootFromContext(void *context) -> file_security_internal::ValidationRoot {
			return context == nullptr
			           ? file_security_internal::ValidationRoot{}
			           : *static_cast<file_security_internal::ValidationRoot *>(context);
		}

		auto IsSafeEditorPath(const fs::path &path) -> bool {
			return path.is_absolute() && fs::is_regular_file(path) &&
			       access(path.c_str(), X_OK) == 0;
		}

		auto ResolveEditor(bool allow_env_editor) -> std::string {
			if (allow_env_editor) {
				if (const char *editor = std::getenv("EDITOR");
				    editor != nullptr && editor[0] != '\0') {
					const fs::path editor_path(editor);
					if (IsSafeEditorPath(editor_path)) {
						return editor_path.string();
					}
				}
			}

			for (const char *candidate : {"/usr/bin/micro", "/usr/bin/nano", "/usr/bin/vi"}) {
				if (access(candidate, X_OK) == 0) {
					return candidate;
				}
			}

			return {};
		}

		void RemoveIfExists(const fs::path &path) {
			std::error_code ec;
			fs::remove(path, ec);
		}

		void ResetEditorEnvironment(const howdy::native::InvokingUser &invoking_user) {
			howdy::native::ResetInvokingUserEnvironment(invoking_user);
		}

		auto CreateTempCopy(const fs::path                                   &source_path,
		                    const std::optional<howdy::native::InvokingUser> &invoking_user,
		                    std::string                                      *source_content,
		                    const file_security_internal::ValidationRoot     &validation_root)
		    -> std::optional<fs::path> {
			if (source_content != nullptr) {
				source_content->clear();
			}

			const int input_fd =
			    open(source_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
			if (input_fd < 0) {
				return std::nullopt;
			}
			if (config_test_hooks::Current()) {
				config_test_hooks::Current()();
			}
			const auto security = howdy::native::CheckSecureConfigFd(
			    input_fd, source_path, DefaultSecureOwnerUid(), validation_root);
			if (!security.ok) {
				close(input_fd);
				return std::nullopt;
			}
			auto content = howdy::native::ReadConfigFromFd(input_fd);
			close(input_fd);
			if (!content.has_value()) {
				return std::nullopt;
			}

			fs::path          temp_dir      = fs::temp_directory_path();
			std::string       temp_template = (temp_dir / "howdy-config-XXXXXX").string();
			std::vector<char> writable(temp_template.begin(), temp_template.end());
			writable.push_back('\0');

			const int fd = mkostemp(writable.data(), O_CLOEXEC);
			if (fd < 0) {
				return std::nullopt;
			}

			fs::path temp_path(writable.data());
			bool     ok = true;

			if (fchmod(fd, S_IRUSR | S_IWUSR) != 0) {
				ok = false;
			}

			if (ok && invoking_user.has_value() &&
			    fchown(fd, invoking_user->uid, invoking_user->gid) != 0) {
				ok = false;
			}

			if (ok && !howdy::native::WriteAllToFd(fd, *content)) {
				ok = false;
			}

			if (ok && !howdy::native::SyncFd(fd)) {
				ok = false;
			}

			if (close(fd) != 0) {
				ok = false;
			}

			if (!ok) {
				RemoveIfExists(temp_path);
				if (source_content != nullptr) {
					source_content->clear();
				}
				return std::nullopt;
			}
			if (source_content != nullptr) {
				*source_content = std::move(*content);
			}

			return temp_path;
		}

		auto RunEditor(const std::string &editor, const fs::path &temp_path,
		               const std::optional<howdy::native::InvokingUser> &invoking_user) -> int {
			const pid_t child_pid = fork();
			if (child_pid < 0) {
				return -1;
			}

			if (child_pid == 0) {
				if (invoking_user.has_value()) {
					if (initgroups(invoking_user->name.c_str(), invoking_user->gid) != 0 ||
					    setgid(invoking_user->gid) != 0 || setuid(invoking_user->uid) != 0) {
						_exit(126);
					}
					if (getuid() != invoking_user->uid || geteuid() != invoking_user->uid ||
					    getgid() != invoking_user->gid || getegid() != invoking_user->gid) {
						_exit(126);
					}
					ResetEditorEnvironment(*invoking_user);
				}

				std::array<char *, 3> exec_argv = {
				    const_cast<char *>(editor.c_str()),
				    const_cast<char *>(temp_path.c_str()),
				    nullptr,
				};
				execv(editor.c_str(), exec_argv.data());
				_exit(127);
			}

			int status = 0;
			while (waitpid(child_pid, &status, 0) < 0) {
				if (errno != EINTR) {
					return -1;
				}
			}
			return status;
		}

		auto ReadTempConfigSnapshot(const fs::path &temp_path, std::string *content) -> bool {
			if (content == nullptr) {
				return false;
			}
			const int input_fd = open(temp_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
			if (input_fd < 0) {
				return false;
			}

			struct stat edited_stat{};
			const bool  edited_ok =
			    fstat(input_fd, &edited_stat) == 0 && S_ISREG(edited_stat.st_mode);
			if (!edited_ok) {
				close(input_fd);
				return false;
			}

			auto snapshot = howdy::native::ReadConfigFromFd(input_fd);
			close(input_fd);
			if (!snapshot.has_value()) {
				return false;
			}
			*content = std::move(*snapshot);
			return true;
		}

		auto FileContentMatches(const fs::path &path, const std::string &expected,
		                        const file_security_internal::ValidationRoot &validation_root)
		    -> bool {
			const int input_fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
			if (input_fd < 0) {
				return false;
			}
			if (config_test_hooks::Current()) {
				config_test_hooks::Current()();
			}
			const auto security = howdy::native::CheckSecureConfigFd(
			    input_fd, path, DefaultSecureOwnerUid(), validation_root);
			if (!security.ok) {
				close(input_fd);
				return false;
			}

			const auto current = howdy::native::ReadConfigFromFd(input_fd);
			close(input_fd);
			return current.has_value() && *current == expected;
		}

		auto ResolveInvokingIdentityDependency(void *context)
		    -> howdy::native::InvokingIdentityResult {
			(void)context;
			return howdy::native::ResolveInvokingIdentity();
		}

		auto ResolveEditorDependency(void *context, bool allow_env_editor) -> std::string {
			(void)context;
			return ResolveEditor(allow_env_editor);
		}

		auto ResolveConfigPathDependency(void *context) -> fs::path {
			(void)context;
			return howdy::native::ResolveConfigPath();
		}

		auto CheckSecureConfigPathDependency(void *context, const fs::path &config_path)
		    -> howdy::native::ConfigPathCheckResult {
			return howdy::native::CheckSecureConfigPath(config_path, DefaultSecureOwnerUid(),
			                                            ValidationRootFromContext(context));
		}

		auto
		CreateTempCopyDependency(void *context, const fs::path &source_path,
		                         const std::optional<howdy::native::InvokingUser> &invoking_user)
		    -> std::optional<howdy::native::config_internal::TempConfigCopy> {
			std::string original_content;
			const auto  path = CreateTempCopy(source_path, invoking_user, &original_content,
			                                  ValidationRootFromContext(context));
			if (!path) {
				return std::nullopt;
			}
			return howdy::native::config_internal::TempConfigCopy{
			    .path = *path, .original_content = std::move(original_content)};
		}

		auto RunEditorDependency(void *context, const std::string &editor,
		                         const fs::path                                   &temp_path,
		                         const std::optional<howdy::native::InvokingUser> &invoking_user)
		    -> int {
			(void)context;
			return RunEditor(editor, temp_path, invoking_user);
		}

		auto ReadTempConfigSnapshotDependency(void *context, const fs::path &temp_path,
		                                      std::string *content) -> bool {
			(void)context;
			return ReadTempConfigSnapshot(temp_path, content);
		}

		auto ValidateConfigContentDependency(void *context, const std::string &content,
		                                     std::string *error_message) -> bool {
			(void)context;
			return howdy::native::ValidateConfigContent(content, error_message);
		}

		auto FileContentMatchesDependency(void *context, const fs::path &path,
		                                  const std::string &expected) -> bool {
			return FileContentMatches(path, expected, ValidationRootFromContext(context));
		}

		auto ReplaceConfigContentAtomicallyDependency(void *context, const fs::path &config_path,
		                                              const std::string &content,
		                                              std::string *error_message, bool lock,
		                                              bool               validate_runtime,
		                                              const std::string *expected_current_content)
		    -> bool {
			return howdy::native::ReplaceConfigContentAtomically(
			    config_path, content, error_message, lock, validate_runtime,
			    expected_current_content, SyncParentDirectory, ValidationRootFromContext(context));
		}

		auto RemoveIfExistsDependency(void *context, const fs::path &path) -> void {
			(void)context;
			RemoveIfExists(path);
		}

	}  // namespace

	ConfigEditSession::ConfigEditSession(ConfigEditDependencies dependencies)
	    : dependencies_(dependencies) {}

	auto ConfigEditDependenciesAvailable(const ConfigEditDependencies &dependencies) -> bool {
		return dependencies.resolve_invoking_identity != nullptr &&
		       dependencies.resolve_editor != nullptr &&
		       dependencies.resolve_config_path != nullptr &&
		       dependencies.check_secure_config_path != nullptr &&
		       dependencies.create_temp_copy != nullptr && dependencies.run_editor != nullptr &&
		       dependencies.read_temp_config_snapshot != nullptr &&
		       dependencies.validate_config_content != nullptr &&
		       dependencies.file_content_matches != nullptr &&
		       dependencies.replace_config_content_atomically != nullptr &&
		       dependencies.remove_if_exists != nullptr;
	}

	auto DefaultConfigEditDependencies(file_security_internal::ValidationRoot *validation_root)
	    -> ConfigEditDependencies {
		return {
		    .context                           = validation_root,
		    .resolve_invoking_identity         = ResolveInvokingIdentityDependency,
		    .resolve_editor                    = ResolveEditorDependency,
		    .resolve_config_path               = ResolveConfigPathDependency,
		    .check_secure_config_path          = CheckSecureConfigPathDependency,
		    .create_temp_copy                  = CreateTempCopyDependency,
		    .run_editor                        = RunEditorDependency,
		    .read_temp_config_snapshot         = ReadTempConfigSnapshotDependency,
		    .validate_config_content           = ValidateConfigContentDependency,
		    .file_content_matches              = FileContentMatchesDependency,
		    .replace_config_content_atomically = ReplaceConfigContentAtomicallyDependency,
		    .remove_if_exists                  = RemoveIfExistsDependency,
		};
	}

	auto ConfigEditSession::Run(const ConfigEditRequest &request) const -> ConfigEditResult {
		if (!ConfigEditDependenciesAvailable(dependencies_)) {
			return {.status = ConfigEditStatus::kDependenciesUnavailable};
		}

		const auto invoking_identity =
		    dependencies_.resolve_invoking_identity(dependencies_.context);
		if (invoking_identity.status == howdy::native::InvokingIdentityStatus::kInvalid) {
			return {.status = ConfigEditStatus::kInvokingIdentityInvalid};
		}
		if (invoking_identity.status == howdy::native::InvokingIdentityStatus::kConflicting) {
			return {.status = ConfigEditStatus::kInvokingIdentityConflicting};
		}
		if (invoking_identity.status != howdy::native::InvokingIdentityStatus::kNoWrapperIdentity &&
		    invoking_identity.status != howdy::native::InvokingIdentityStatus::kResolved) {
			return {.status = ConfigEditStatus::kInvokingIdentityInvalid};
		}
		if ((invoking_identity.status ==
		         howdy::native::InvokingIdentityStatus::kNoWrapperIdentity &&
		     invoking_identity.user.has_value()) ||
		    (invoking_identity.status == howdy::native::InvokingIdentityStatus::kResolved &&
		     !invoking_identity.user.has_value())) {
			return {.status = ConfigEditStatus::kInvokingIdentityInvalid};
		}

		const auto invoking_user = invoking_identity.user;
		const bool allow_env_editor =
		    invoking_identity.status == howdy::native::InvokingIdentityStatus::kResolved &&
		    invoking_user.has_value() && invoking_user->uid != 0;
		const auto editor = dependencies_.resolve_editor(dependencies_.context, allow_env_editor);
		if (editor.empty()) {
			return {.status = ConfigEditStatus::kEditorUnavailable};
		}

		const auto config_path = dependencies_.resolve_config_path(dependencies_.context);
		const auto config_security =
		    dependencies_.check_secure_config_path(dependencies_.context, config_path);
		if (!config_security.ok) {
			return {
			    .status = ConfigEditStatus::kSecurityCheckFailed,
			    .error  = config_security.error_message,
			    .editor = editor,
			};
		}

		const auto temp_copy =
		    dependencies_.create_temp_copy(dependencies_.context, config_path, invoking_user);
		if (!temp_copy) {
			return {.status = ConfigEditStatus::kTempCreateFailed, .editor = editor};
		}

		const auto cleanup = [&]() -> void {
			dependencies_.remove_if_exists(dependencies_.context, temp_copy->path);
		};
		const auto result_base = [&](ConfigEditStatus status,
		                             std::string      error = {}) -> ConfigEditResult {
			return ConfigEditResult{
			    .status    = status,
			    .error     = std::move(error),
			    .temp_path = temp_copy->path,
			    .editor    = editor,
			};
		};

		if (request.editor_ready != nullptr) {
			request.editor_ready(request.context, editor);
		}

		const int status =
		    dependencies_.run_editor(dependencies_.context, editor, temp_copy->path, invoking_user);
		if (status < 0) {
			cleanup();
			return result_base(ConfigEditStatus::kEditorLaunchFailed);
		}

		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
			cleanup();
			return result_base(ConfigEditStatus::kEditorFailed);
		}

		std::string edited_content;
		if (!dependencies_.read_temp_config_snapshot(dependencies_.context, temp_copy->path,
		                                             &edited_content)) {
			cleanup();
			return result_base(ConfigEditStatus::kReadFailed);
		}

		std::string validation_error;
		if (!dependencies_.validate_config_content(dependencies_.context, edited_content,
		                                           &validation_error)) {
			return result_base(ConfigEditStatus::kInvalidEditedConfig, std::move(validation_error));
		}

		if (dependencies_.file_content_matches(dependencies_.context, config_path,
		                                       edited_content)) {
			cleanup();
			return result_base(ConfigEditStatus::kNoChanges);
		}

		std::string install_error;
		if (!dependencies_.replace_config_content_atomically(dependencies_.context, config_path,
		                                                     edited_content, &install_error, true,
		                                                     false, &temp_copy->original_content)) {
			cleanup();
			const auto result_status =
			    install_error == howdy::native::kStaleEditedConfigMessage ||
			            install_error == "Config changed while editor was open"
			        ? ConfigEditStatus::kConfigChanged
			        : ConfigEditStatus::kInstallFailed;
			return result_base(result_status, std::move(install_error));
		}

		cleanup();
		return result_base(ConfigEditStatus::kOk);
	}

}  // namespace howdy::native::config_internal
