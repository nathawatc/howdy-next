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
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <sys/wait.h>

namespace howdy::native::config_internal {

	namespace {

		namespace fs = std::filesystem;

		constexpr std::array<const char *, 3> kFallbackEditors = {"micro", "nano", "vi"};

		enum class EditorLaunchReport : unsigned char {
			kUnavailable = 1,
			kLaunchFailed,
		};

		auto IsSupportedEditorPreference(std::string_view editor) -> bool {
			return !editor.empty() && editor.find_first_of(" \t\n\r\v\f") == std::string_view::npos;
		}

		auto IsEditorUnavailableError(int error) -> bool {
			switch (error) {
				case EACCES:
				case EISDIR:
				case ELOOP:
				case ENAMETOOLONG:
				case ENOENT:
				case ENOEXEC:
				case ENOTDIR:
					return true;
				default:
					return false;
			}
		}

		auto ExecEditorThroughPath(const std::string &editor, const fs::path &temp_path) -> int {
			std::array<char *, 3> exec_argv = {
			    const_cast<char *>(editor.c_str()),
			    const_cast<char *>(temp_path.c_str()),
			    nullptr,
			};

			// execvp may invoke /bin/sh for ENOEXEC. Search with execv so editor execution stays
			// direct.
			if (editor.contains('/')) {
				execv(editor.c_str(), exec_argv.data());
				return errno;
			}

			std::vector<char> default_path;
			std::string_view  search_path;
			if (const char *path = std::getenv("PATH"); path != nullptr) {
				search_path = path;
			} else {
				const auto path_size = confstr(_CS_PATH, nullptr, 0);
				if (path_size == 0) {
					return ENOENT;
				}
				default_path.resize(path_size);
				const auto written = confstr(_CS_PATH, default_path.data(), default_path.size());
				if (written == 0 || written > default_path.size()) {
					return ENOENT;
				}
				search_path = std::string_view(default_path.data(), written - 1);
			}

			int         last_error = ENOENT;
			std::size_t start      = 0;
			while (true) {
				const auto end       = search_path.find(':', start);
				const auto directory = search_path.substr(
				    start, end == std::string_view::npos ? std::string_view::npos : end - start);

				std::string candidate;
				if (directory.empty()) {
					candidate = editor;
				} else {
					candidate.reserve(directory.size() + 1 + editor.size());
					candidate.append(directory);
					candidate.push_back('/');
					candidate.append(editor);
				}

				execv(candidate.c_str(), exec_argv.data());
				const int error = errno;
				if (!IsEditorUnavailableError(error)) {
					return error;
				}
				if (error == EACCES) {
					last_error = EACCES;
				}

				if (end == std::string_view::npos) {
					break;
				}
				start = end + 1;
			}

			return last_error;
		}

		void RemoveIfExists(const fs::path &path) {
			std::error_code ec;
			fs::remove(path, ec);
		}

		[[noreturn]] auto ReportEditorLaunch(int report_fd, EditorLaunchReport report,
		                                     int exit_code) -> void {
			const auto report_byte = static_cast<unsigned char>(report);
			ssize_t    written;
			do {
				written = write(report_fd, &report_byte, sizeof(report_byte));
			} while (written < 0 && errno == EINTR);
			(void)written;
			_exit(exit_code);
		}

		auto TryEditor(const std::string &editor, const fs::path &temp_path, int report_fd)
		    -> void {
			if (!IsEditorUnavailableError(ExecEditorThroughPath(editor, temp_path))) {
				ReportEditorLaunch(report_fd, EditorLaunchReport::kLaunchFailed, 126);
			}
		}

		[[noreturn]] auto
		RunEditorChild(const std::string &editor_preference, const fs::path &temp_path,
		               const std::optional<howdy::native::InvokingUser> &invoking_user,
		               int                                               report_fd) -> void {
			if (invoking_user.has_value()) {
				if (initgroups(invoking_user->name.c_str(), invoking_user->gid) != 0 ||
				    setgid(invoking_user->gid) != 0 || setuid(invoking_user->uid) != 0) {
					ReportEditorLaunch(report_fd, EditorLaunchReport::kLaunchFailed, 126);
				}
				if (getuid() != invoking_user->uid || geteuid() != invoking_user->uid ||
				    getgid() != invoking_user->gid || getegid() != invoking_user->gid) {
					ReportEditorLaunch(report_fd, EditorLaunchReport::kLaunchFailed, 126);
				}
				howdy::native::ResetInvokingUserEnvironment(*invoking_user);
			}

			TryEditor(editor_preference, temp_path, report_fd);
			for (const char *fallback : kFallbackEditors) {
				if (editor_preference != fallback) {
					TryEditor(fallback, temp_path, report_fd);
				}
			}
			ReportEditorLaunch(report_fd, EditorLaunchReport::kUnavailable, 127);
		}

		auto RunEditor(const std::string &editor_preference, const fs::path &temp_path,
		               const std::optional<howdy::native::InvokingUser> &invoking_user) -> int {
			std::array<int, 2> report_pipe{};
			if (pipe2(report_pipe.data(), O_CLOEXEC) != 0) {
				return -1;
			}

			const pid_t child_pid = fork();
			if (child_pid < 0) {
				close(report_pipe.at(0));
				close(report_pipe.at(1));
				return -1;
			}

			if (child_pid == 0) {
				close(report_pipe.at(0));
				RunEditorChild(editor_preference, temp_path, invoking_user, report_pipe.at(1));
			}

			close(report_pipe.at(1));
			int status = 0;
			while (waitpid(child_pid, &status, 0) < 0) {
				if (errno != EINTR) {
					close(report_pipe.at(0));
					return -1;
				}
			}

			unsigned char report_byte = 0;
			ssize_t       read_count;
			do {
				read_count = read(report_pipe.at(0), &report_byte, sizeof(report_byte));
			} while (read_count < 0 && errno == EINTR);
			close(report_pipe.at(0));
			if (read_count < 0) {
				return -1;
			}
			if (read_count == 1) {
				switch (static_cast<EditorLaunchReport>(report_byte)) {
					case EditorLaunchReport::kUnavailable:
						return kEditorUnavailableRunResult;
					case EditorLaunchReport::kLaunchFailed:
						return -1;
				}
				return -1;
			}
			return status;
		}

		auto ConfigEditDependenciesAvailable(const ConfigEditDependencies &dependencies) -> bool {
			return dependencies.resolve_invoking_identity != nullptr &&
			       dependencies.select_editor_preference != nullptr &&
			       dependencies.run_editor != nullptr;
		}

		auto ResolveInvokingIdentityDependency(void *context)
		    -> howdy::native::InvokingIdentityResult {
			(void)context;
			return howdy::native::ResolveInvokingIdentity();
		}

		auto SelectEditorPreferenceDependency(void *context) -> std::string {
			(void)context;
			return SelectEditorPreference();
		}

		auto RunEditorDependency(void *context, const std::string &editor_preference,
		                         const fs::path                                   &temp_path,
		                         const std::optional<howdy::native::InvokingUser> &invoking_user)
		    -> int {
			(void)context;
			return RunEditor(editor_preference, temp_path, invoking_user);
		}

	}  // namespace

	auto SelectEditorPreference() -> std::string {
		if (const char *editor = std::getenv("EDITOR");
		    editor != nullptr && IsSupportedEditorPreference(editor)) {
			return editor;
		}

		return kFallbackEditors.front();
	}

	auto CreateTempConfigCopy(const fs::path                                   &source_path,
	                          const std::optional<howdy::native::InvokingUser> &invoking_user,
	                          const file_security_internal::ValidationRoot     &validation_root)
	    -> std::optional<TempConfigCopy> {
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
			return std::nullopt;
		}

		return TempConfigCopy{
		    .path             = std::move(temp_path),
		    .original_content = std::move(*content),
		};
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
		const bool  edited_ok = fstat(input_fd, &edited_stat) == 0 && S_ISREG(edited_stat.st_mode);
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
	                        const file_security_internal::ValidationRoot &validation_root) -> bool {
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

	ConfigEditSession::ConfigEditSession(ConfigEditDependencies dependencies)
	    : dependencies_(std::move(dependencies)) {}

	auto DefaultConfigEditDependencies(file_security_internal::ValidationRoot validation_root)
	    -> ConfigEditDependencies {
		return {
		    .context                   = nullptr,
		    .resolve_invoking_identity = ResolveInvokingIdentityDependency,
		    .select_editor_preference  = SelectEditorPreferenceDependency,
		    .run_editor                = RunEditorDependency,
		    .validation_root           = std::move(validation_root),
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
		const auto editor        = dependencies_.select_editor_preference(dependencies_.context);
		if (editor.empty()) {
			return {.status = ConfigEditStatus::kEditorUnavailable};
		}

		const auto config_path     = howdy::native::ResolveConfigPath();
		const auto config_security = howdy::native::CheckSecureConfigPath(
		    config_path, DefaultSecureOwnerUid(), dependencies_.validation_root);
		if (!config_security.ok) {
			return {
			    .status = ConfigEditStatus::kSecurityCheckFailed,
			    .error  = config_security.error_message,
			    .editor = editor,
			};
		}

		const auto temp_copy =
		    CreateTempConfigCopy(config_path, invoking_user, dependencies_.validation_root);
		if (!temp_copy) {
			return {.status = ConfigEditStatus::kTempCreateFailed, .editor = editor};
		}

		const auto cleanup = [&]() -> void {
			RemoveIfExists(temp_copy->path);
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
			request.editor_ready(request.context);
		}

		const int status =
		    dependencies_.run_editor(dependencies_.context, editor, temp_copy->path, invoking_user);
		if (status == kEditorUnavailableRunResult) {
			cleanup();
			return result_base(ConfigEditStatus::kEditorUnavailable);
		}
		if (status < 0) {
			cleanup();
			return result_base(ConfigEditStatus::kEditorLaunchFailed);
		}

		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
			cleanup();
			return result_base(ConfigEditStatus::kEditorFailed);
		}

		std::string edited_content;
		if (!ReadTempConfigSnapshot(temp_copy->path, &edited_content)) {
			cleanup();
			return result_base(ConfigEditStatus::kReadFailed);
		}

		std::string validation_error;
		if (!howdy::native::ValidateConfigContent(edited_content, &validation_error)) {
			return result_base(ConfigEditStatus::kInvalidEditedConfig, std::move(validation_error));
		}

		if (FileContentMatches(config_path, edited_content, dependencies_.validation_root)) {
			cleanup();
			return result_base(ConfigEditStatus::kNoChanges);
		}

		std::string install_error;
		if (!howdy::native::ReplaceConfigContentAtomically(
		        config_path, edited_content, &install_error, true, false,
		        &temp_copy->original_content, SyncParentDirectory, dependencies_.validation_root)) {
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
