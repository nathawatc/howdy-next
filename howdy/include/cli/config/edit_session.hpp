#pragma once

#include "support/file_security/validation_root.hpp"
#include "support/invoking_user.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>

#include <sys/types.h>

namespace howdy::native::config_internal {

	inline constexpr std::array<const char *, 3> kFallbackEditors = {"micro", "nano", "vi"};

	struct TempConfigCopy {
		std::filesystem::path path;
		std::string           original_content;
	};

	enum class ConfigEditStatus : std::uint8_t {
		kDependenciesUnavailable,
		kInvokingIdentityInvalid,
		kInvokingIdentityConflicting,
		kOk,
		kNoChanges,
		kEditorUnavailable,
		kSecurityCheckFailed,
		// The config could not be opened or read.
		kConfigUnreadable,
		kTempCreateFailed,
		kEditorLaunchFailed,
		kEditorFailed,
		// The editor did not succeed and the copy may hold edits (or its result was lost, and the
		// editor may still be running), so the copy is kept.
		kEditorFailedEditKept,
		kReadFailed,
		kEditedConfigUnreadable,
		kInvalidEditedConfig,
		kConfigChanged,
		kInstallFailed,
	};

	enum class EditorRunResult : std::uint8_t {
		kLaunchFailed,
		kUnavailable,  // no editor candidate could be executed
		kSucceeded,
		kExitedWithError,
		kKilledBySignal,
		// The editor ran, but how it ended could not be learnt.
		kResultLost,
	};

	using EditorReadyFn = void (*)(void *context);

	struct ConfigEditRequest {
		void         *context      = nullptr;
		EditorReadyFn editor_ready = nullptr;
	};

	struct ConfigEditResult {
		ConfigEditStatus      status = ConfigEditStatus::kInstallFailed;
		std::string           error;
		std::filesystem::path temp_path;
		std::string           editor;
	};

	using ResolveInvokingIdentityFn = howdy::native::InvokingIdentityResult (*)(void *context);
	using SelectEditorPreferenceFn  = std::string (*)(void *context);
	using RunEditorFn               = EditorRunResult (*)(
	    void *context, const std::string &editor, const std::filesystem::path &temp_path,
	    const std::optional<howdy::native::InvokingUser> &invoking_user);

	struct ConfigEditDependencies {
		void                                  *context                   = nullptr;
		ResolveInvokingIdentityFn              resolve_invoking_identity = nullptr;
		SelectEditorPreferenceFn               select_editor_preference  = nullptr;
		RunEditorFn                            run_editor                = nullptr;
		file_security_internal::ValidationRoot validation_root{};
	};

	class ConfigEditSession {
	public:
		explicit ConfigEditSession(ConfigEditDependencies dependencies);

		[[nodiscard]] auto Run(const ConfigEditRequest &request) const -> ConfigEditResult;

	private:
		ConfigEditDependencies dependencies_;
	};

	[[nodiscard]] auto
	DefaultConfigEditDependencies(file_security_internal::ValidationRoot validation_root = {})
	    -> ConfigEditDependencies;

	[[nodiscard]] auto SelectEditorPreference() -> std::string;

	[[nodiscard]] auto
	CreateTempConfigCopy(const std::filesystem::path                      &source_path,
	                     const std::optional<howdy::native::InvokingUser> &invoking_user,
	                     const file_security_internal::ValidationRoot     &validation_root = {})
	    -> std::optional<TempConfigCopy>;

	enum class TempConfigReadStatus : std::uint8_t {
		kOk,
		kUntrusted,   // missing, or not a regular file of the expected owner
		kUnreadable,  // may be the user's own file, but too large or failing to open or read
	};

	[[nodiscard]] auto ReadTempConfigSnapshot(const std::filesystem::path &temp_path,
	                                          uid_t expected_owner, std::string *content)
	    -> TempConfigReadStatus;

	struct CurrentConfigError {
		bool        insecure = false;  // or not opened or read
		std::string message;
	};

	// Or why the config cannot be opened, trusted or read.
	[[nodiscard]] auto
	ReadCurrentConfig(const std::filesystem::path                  &path,
	                  const file_security_internal::ValidationRoot &validation_root = {})
	    -> std::expected<std::string, CurrentConfigError>;

}  // namespace howdy::native::config_internal
