#pragma once

#include "config/config_utils.hpp"
#include "support/invoking_user.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace howdy::native::config_internal {

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
		kTempCreateFailed,
		kEditorLaunchFailed,
		kEditorFailed,
		kReadFailed,
		kInvalidEditedConfig,
		kConfigChanged,
		kInstallFailed,
	};

	using EditorReadyFn = void (*)(void *context, const std::string &editor);

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
	using ResolveEditorFn           = std::string (*)(void *context, bool allow_env_editor);
	using ResolveConfigPathFn       = std::filesystem::path (*)(void *context);
	using CheckSecureConfigPathFn   = howdy::native::ConfigPathCheckResult (*)(
	    void *context, const std::filesystem::path &config_path);
	using CreateTempCopyFn = std::optional<TempConfigCopy> (*)(
	    void *context, const std::filesystem::path &source_path,
	    const std::optional<howdy::native::InvokingUser> &invoking_user);
	using RunEditorFn = int (*)(void *context, const std::string &editor,
	                            const std::filesystem::path                      &temp_path,
	                            const std::optional<howdy::native::InvokingUser> &invoking_user);
	using ReadTempConfigSnapshotFn = bool (*)(void *context, const std::filesystem::path &temp_path,
	                                          std::string *content);
	using ValidateConfigContentFn  = bool (*)(void *context, const std::string &content,
	                                          std::string *error_message);
	using FileContentMatchesFn     = bool (*)(void *context, const std::filesystem::path &path,
	                                          const std::string &expected);
	using ReplaceConfigContentAtomicallyFn = bool (*)(void                        *context,
	                                                  const std::filesystem::path &config_path,
	                                                  const std::string           &content,
	                                                  std::string *error_message, bool lock,
	                                                  bool               validate_runtime,
	                                                  const std::string *expected_current_content);
	using RemoveIfExistsFn = void (*)(void *context, const std::filesystem::path &path);

	struct ConfigEditDependencies {
		void                            *context                           = nullptr;
		ResolveInvokingIdentityFn        resolve_invoking_identity         = nullptr;
		ResolveEditorFn                  resolve_editor                    = nullptr;
		ResolveConfigPathFn              resolve_config_path               = nullptr;
		CheckSecureConfigPathFn          check_secure_config_path          = nullptr;
		CreateTempCopyFn                 create_temp_copy                  = nullptr;
		RunEditorFn                      run_editor                        = nullptr;
		ReadTempConfigSnapshotFn         read_temp_config_snapshot         = nullptr;
		ValidateConfigContentFn          validate_config_content           = nullptr;
		FileContentMatchesFn             file_content_matches              = nullptr;
		ReplaceConfigContentAtomicallyFn replace_config_content_atomically = nullptr;
		RemoveIfExistsFn                 remove_if_exists                  = nullptr;
	};

	class ConfigEditSession {
	public:
		explicit ConfigEditSession(ConfigEditDependencies dependencies);

		[[nodiscard]] auto Run(const ConfigEditRequest &request) const -> ConfigEditResult;

	private:
		ConfigEditDependencies dependencies_;
	};

	[[nodiscard]] auto ConfigEditDependenciesAvailable(const ConfigEditDependencies &dependencies)
	    -> bool;
	[[nodiscard]] auto
	DefaultConfigEditDependencies(file_security_internal::ValidationRoot *validation_root = nullptr)
	    -> ConfigEditDependencies;

}  // namespace howdy::native::config_internal
