#include "cli/config/edit_session.hpp"

#include "config/config_utils.hpp"
#include "config/runtime_paths.hpp"
#include "internal.hpp"
#include "support/atomic_files.hpp"
#include "support/fd_io.hpp"
#include "support/file_security.hpp"
#include "support/file_security/validation_root.hpp"
#include "support/invoking_user.hpp"
#include "support/scoped_fd.hpp"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>

namespace howdy::native::config_internal {

	namespace {

		namespace fs = std::filesystem;

		auto IsSupportedEditorPreference(std::string_view editor) -> bool {
			return !editor.empty() && editor.find_first_of(" \t\n\r\v\f") == std::string_view::npos;
		}

		void RemoveIfExists(const fs::path &path) {
			std::error_code ec;
			fs::remove(path, ec);
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
		const auto opened = howdy::native::OpenSecureConfigForRead(
		    source_path, DefaultSecureOwnerUid(), validation_root);
		if (!opened.fd.Valid() || !opened.security.ok) {
			return std::nullopt;
		}
		auto content = howdy::native::ReadConfigFromFd(opened.fd.Get());
		if (!content.has_value()) {
			return std::nullopt;
		}

		fs::path const    temp_dir      = fs::temp_directory_path();
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

	auto ReadTempConfigSnapshot(const fs::path &temp_path, uid_t expected_owner,
	                            std::string *content) -> bool {
		if (content == nullptr) {
			return false;
		}
		const ScopedFd input_fd(open(temp_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
		if (!input_fd.Valid()) {
			return false;
		}

		struct stat edited_stat{};
		// The invoking user controls the temp path; a hard link to a root-owned file fails here.
		const bool edited_ok = fstat(input_fd.Get(), &edited_stat) == 0 &&
		                       S_ISREG(edited_stat.st_mode) && edited_stat.st_uid == expected_owner;
		if (!edited_ok) {
			return false;
		}

		auto snapshot = howdy::native::ReadConfigFromFd(input_fd.Get());
		if (!snapshot.has_value()) {
			return false;
		}
		*content = std::move(*snapshot);
		return true;
	}

	auto FileContentMatches(const fs::path &path, const std::string &expected,
	                        const file_security_internal::ValidationRoot &validation_root) -> bool {
		const auto opened =
		    howdy::native::OpenSecureConfigForRead(path, DefaultSecureOwnerUid(), validation_root);
		if (!opened.fd.Valid() || !opened.security.ok) {
			return false;
		}

		const auto current = howdy::native::ReadConfigFromFd(opened.fd.Get());
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

		const auto cleanup = [&] -> void {
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
		const uid_t temp_owner = invoking_user.has_value() ? invoking_user->uid : geteuid();
		if (!ReadTempConfigSnapshot(temp_copy->path, temp_owner, &edited_content)) {
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
			// Keep the valid edits: the install was refused or may not be durable.
			const auto result_status = install_error == howdy::native::kStaleEditedConfigMessage
			                               ? ConfigEditStatus::kConfigChanged
			                               : ConfigEditStatus::kInstallFailed;
			return result_base(result_status, std::move(install_error));
		}

		cleanup();
		return result_base(ConfigEditStatus::kOk);
	}

}  // namespace howdy::native::config_internal
