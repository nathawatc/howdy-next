#pragma once

#include "support/file_security/validation_root.hpp"
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

	// RunEditorFn returns this sentinel when every editor candidate was unavailable.
	inline constexpr int kEditorUnavailableRunResult = -2;

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
	using RunEditorFn = int (*)(void *context, const std::string &editor,
	                            const std::filesystem::path                      &temp_path,
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

	auto SelectEditorPreference() -> std::string;

	auto CreateTempConfigCopy(const std::filesystem::path                      &source_path,
	                          const std::optional<howdy::native::InvokingUser> &invoking_user,
	                          const file_security_internal::ValidationRoot &validation_root = {})
	    -> std::optional<TempConfigCopy>;

	auto ReadTempConfigSnapshot(const std::filesystem::path &temp_path, std::string *content)
	    -> bool;

	auto FileContentMatches(const std::filesystem::path &path, const std::string &expected,
	                        const file_security_internal::ValidationRoot &validation_root = {})
	    -> bool;

}  // namespace howdy::native::config_internal
