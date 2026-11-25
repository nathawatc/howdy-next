#include "cli/config/edit_session.hpp"

#include "config/config_limits.hpp"
#include "config/config_utils.hpp"
#include "config/runtime_paths.hpp"
#include "internal.hpp"
#include "support/fd_io.hpp"
#include "support/file_security.hpp"
#include "support/file_security/validation_root.hpp"
#include "support/invoking_user.hpp"
#include "support/scoped_fd.hpp"

#include <cerrno>
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

		class TempFileGuard {
		public:
			explicit TempFileGuard(fs::path path) noexcept
			    : path_(std::move(path)) {}

			~TempFileGuard() {
				if (armed_) {
					RemoveIfExists(path_);
				}
			}

			TempFileGuard(const TempFileGuard &)                     = delete;
			auto operator=(const TempFileGuard &) -> TempFileGuard & = delete;
			TempFileGuard(TempFileGuard &&)                          = delete;
			auto operator=(TempFileGuard &&) -> TempFileGuard &      = delete;

			void Dismiss() noexcept {
				armed_ = false;
			}

		private:
			fs::path path_;
			bool     armed_ = true;
		};

		auto ConfigEditDependenciesAvailable(const ConfigEditDependencies &dependencies) -> bool {
			return dependencies.resolve_invoking_identity != nullptr &&
			       dependencies.select_editor_preference != nullptr &&
			       dependencies.run_editor != nullptr;
		}

		auto ResolveInvokingIdentityDependency([[maybe_unused]] void *context)
		    -> howdy::native::InvokingIdentityResult {
			return howdy::native::ResolveInvokingIdentity();
		}

		auto SelectEditorPreferenceDependency([[maybe_unused]] void *context) -> std::string {
			return SelectEditorPreference();
		}

		auto RunEditorDependency([[maybe_unused]] void *context,
		                         const std::string &editor_preference, const fs::path &temp_path,
		                         const std::optional<howdy::native::InvokingUser> &invoking_user)
		    -> EditorRunResult {
			return RunEditor(editor_preference, temp_path, invoking_user);
		}

		auto EditorFailureStatus(EditorRunResult result) -> ConfigEditStatus {
			switch (result) {
				case EditorRunResult::kUnavailable:
					return ConfigEditStatus::kEditorUnavailable;
				case EditorRunResult::kExitedWithError:
				case EditorRunResult::kKilledBySignal:
					return ConfigEditStatus::kEditorFailed;
				case EditorRunResult::kLaunchFailed:
				case EditorRunResult::kSucceeded:
					break;
			}
			return ConfigEditStatus::kEditorLaunchFailed;
		}

		// A parse failure has no detail to add to what its status says.
		auto InvalidEditDetail(howdy::native::ConfigValidationResult validation) -> std::string {
			if (validation.status == howdy::native::ConfigValidationStatus::kUnparsable) {
				return {};
			}
			return std::move(validation.message);
		}

		auto InstallFailureStatus(howdy::native::ConfigWriteStatus status) -> ConfigEditStatus {
			return status == howdy::native::ConfigWriteStatus::kStale
			           ? ConfigEditStatus::kConfigChanged
					   : ConfigEditStatus::kInstallFailed;
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

		std::error_code temp_dir_error;
		fs::path const  temp_dir = fs::temp_directory_path(temp_dir_error);
		if (temp_dir_error || temp_dir.empty()) {
			return std::nullopt;
		}
		std::string       temp_template = (temp_dir / "howdy-config-XXXXXX").string();
		std::vector<char> writable(temp_template.begin(), temp_template.end());
		writable.push_back('\0');

		ScopedFd temp_fd(mkostemp(writable.data(), O_CLOEXEC));
		if (!temp_fd.Valid()) {
			return std::nullopt;
		}

		fs::path      temp_path(writable.data());
		TempFileGuard temp_guard(temp_path);
		bool          ok = true;

		if (fchmod(temp_fd.Get(), S_IRUSR | S_IWUSR) != 0) {
			ok = false;
		}

		if (ok && invoking_user.has_value() &&
		    fchown(temp_fd.Get(), invoking_user->uid, invoking_user->gid) != 0) {
			ok = false;
		}

		if (ok && !howdy::native::WriteAllToFd(temp_fd.Get(), *content)) {
			ok = false;
		}

		if (ok && !howdy::native::SyncFd(temp_fd.Get())) {
			ok = false;
		}

		if (!temp_fd.Close()) {
			ok = false;
		}

		if (!ok) {
			return std::nullopt;
		}

		temp_guard.Dismiss();
		return TempConfigCopy{
		    .path             = std::move(temp_path),
		    .original_content = std::move(*content),
		};
	}

	auto ReadTempConfigSnapshot(const fs::path &temp_path, uid_t expected_owner,
	                            std::string *content) -> TempConfigReadStatus {
		if (content == nullptr) {
			return TempConfigReadStatus::kUntrusted;
		}
		// O_NONBLOCK: a FIFO left at the user-controlled path must not block before S_ISREG.
		const ScopedFd input_fd(
		    open(temp_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
		if (!input_fd.Valid()) {
			// Only these say that the path does not name the user's own file; any other failure
			// may hide valid edits.
			const bool untrusted =
			    errno == ENOENT || errno == ENOTDIR || errno == ELOOP || errno == ENXIO;
			return untrusted ? TempConfigReadStatus::kUntrusted : TempConfigReadStatus::kUnreadable;
		}

		struct stat edited_stat{};
		// The invoking user controls the temp path; a hard link to a root-owned file fails here.
		if (fstat(input_fd.Get(), &edited_stat) != 0) {
			return TempConfigReadStatus::kUnreadable;
		}
		const bool edited_ok = S_ISREG(edited_stat.st_mode) && edited_stat.st_uid == expected_owner;
		if (!edited_ok) {
			return TempConfigReadStatus::kUntrusted;
		}

		auto snapshot = howdy::native::ReadConfigFromFd(input_fd.Get());
		if (!snapshot.has_value()) {
			return TempConfigReadStatus::kUnreadable;
		}
		*content = std::move(*snapshot);
		return TempConfigReadStatus::kOk;
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

		TempFileGuard temp_guard(temp_copy->path);
		const auto    result_base = [&](ConfigEditStatus status,
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

		const auto run_result =
		    dependencies_.run_editor(dependencies_.context, editor, temp_copy->path, invoking_user);
		if (run_result != EditorRunResult::kSucceeded) {
			return result_base(EditorFailureStatus(run_result));
		}

		// The file now holds the user's edits: remove it only on the explicit paths below, so an
		// exception cannot discard them.
		temp_guard.Dismiss();

		std::string edited_content;
		const uid_t temp_owner = invoking_user.has_value() ? invoking_user->uid : geteuid();
		switch (ReadTempConfigSnapshot(temp_copy->path, temp_owner, &edited_content)) {
			case TempConfigReadStatus::kOk:
				break;
			case TempConfigReadStatus::kUnreadable:
				return result_base(ConfigEditStatus::kEditedConfigUnreadable,
				                   "Edited config could not be read or exceeds " +
				                       std::to_string(kMaxConfigFileSize) + " bytes");
			case TempConfigReadStatus::kUntrusted:
				RemoveIfExists(temp_copy->path);
				return result_base(ConfigEditStatus::kReadFailed);
		}

		if (auto validation = howdy::native::ValidateConfigContent(edited_content);
		    validation.status != howdy::native::ConfigValidationStatus::kOk) {
			return result_base(ConfigEditStatus::kInvalidEditedConfig,
			                   InvalidEditDetail(std::move(validation)));
		}

		if (FileContentMatches(config_path, edited_content, dependencies_.validation_root)) {
			RemoveIfExists(temp_copy->path);
			return result_base(ConfigEditStatus::kNoChanges);
		}

		if (auto installed = howdy::native::ReplaceConfigContentAtomically(
		        config_path, edited_content,
		        {
		            .validate_runtime         = false,
		            .expected_current_content = &temp_copy->original_content,
		            .validation_root          = dependencies_.validation_root,
		        });
		    installed.status != howdy::native::ConfigWriteStatus::kOk) {
			// Keep the valid edits: the install was refused or may not be durable.
			return result_base(InstallFailureStatus(installed.status),
			                   std::move(installed.message));
		}

		RemoveIfExists(temp_copy->path);
		return result_base(ConfigEditStatus::kOk);
	}

}  // namespace howdy::native::config_internal
