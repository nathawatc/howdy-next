#include "cli/config.hpp"

#include "cli/config/internal.hpp"
#include "config/config_utils.hpp"

#include <iostream>
#include <string>

namespace {

	constexpr int kConfigExitOk    = 0;
	constexpr int kConfigExitAbort = 1;

	void PrintEditorReady(void *context) {
		(void)context;
		std::cout << "Editing config.ini\n";
	}

	void PrintConfigInstallError(const howdy::native::config_internal::ConfigEditResult &result) {
		if (result.error == howdy::native::kUpdatedConfigInvalidMessage) {
			std::cout << "Edited config is invalid and was not installed: "
			          << result.temp_path.string() << "\n";
		} else if (!result.error.empty()) {
			std::cout << result.error << "\n";
		} else {
			std::cout << howdy::native::kEditedConfigInstallFailedMessage << '\n';
		}
	}

}  // namespace

auto howdy::native::config_internal::ConfigMainWithDependencies(
    const howdy::native::CommandInvocation &invocation, const ConfigDependencies &dependencies)
    -> int {
	(void)invocation;
	if (!ConfigEditDependenciesAvailable(dependencies)) {
		return kConfigExitAbort;
	}

	const ConfigEditSession session(dependencies);
	const auto result = session.Run(ConfigEditRequest{.editor_ready = PrintEditorReady});

	switch (result.status) {
		case ConfigEditStatus::kDependenciesUnavailable:
			return kConfigExitAbort;
		case ConfigEditStatus::kInvokingIdentityInvalid:
			std::cout << "Invalid privilege-wrapper identity; config edit aborted\n";
			return kConfigExitAbort;
		case ConfigEditStatus::kInvokingIdentityConflicting:
			std::cout << "Conflicting privilege-wrapper identity; config edit aborted\n";
			return kConfigExitAbort;
		case ConfigEditStatus::kOk:
			std::cout << "Config updated\n";
			return kConfigExitOk;
		case ConfigEditStatus::kNoChanges:
			std::cout << "No config changes made\n";
			return kConfigExitOk;
		case ConfigEditStatus::kEditorUnavailable:
			std::cout << "Error: No suitable text editor found.\n";
			std::cout << "Set EDITOR to an executable name or path, or install one of: micro, "
			             "nano, vi.\n";
			return kConfigExitAbort;
		case ConfigEditStatus::kSecurityCheckFailed:
			std::cout << result.error << "\n";
			return kConfigExitAbort;
		case ConfigEditStatus::kTempCreateFailed:
			std::cout << "Failed to prepare a temporary config copy\n";
			return kConfigExitAbort;
		case ConfigEditStatus::kEditorLaunchFailed:
			std::cout << "Failed to launch editor\n";
			return kConfigExitAbort;
		case ConfigEditStatus::kEditorFailed:
			std::cout << "Editor exited unsuccessfully; config not updated\n";
			return kConfigExitAbort;
		case ConfigEditStatus::kReadFailed:
			std::cout << howdy::native::kEditedConfigInstallFailedMessage << '\n';
			return kConfigExitAbort;
		case ConfigEditStatus::kInvalidEditedConfig:
		case ConfigEditStatus::kConfigChanged:
		case ConfigEditStatus::kInstallFailed:
			PrintConfigInstallError(result);
			return kConfigExitAbort;
	}

	return kConfigExitAbort;
}

auto ConfigMain(const howdy::native::CommandInvocation &invocation) -> int {
	return howdy::native::config_internal::ConfigMainWithDependencies(
	    invocation, howdy::native::config_internal::DefaultConfigEditDependencies());
}
