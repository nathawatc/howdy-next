#include "cli/config.hpp"

#include "cli/config/internal.hpp"
#include "config/config_utils.hpp"

#include <filesystem>
#include <iostream>
#include <string>

namespace {

	namespace fs = std::filesystem;

	constexpr int kExitOk    = 0;
	constexpr int kExitAbort = 1;

	void print_editor_ready(void *context, const std::string &editor) {
		(void)context;
		std::cout << "Editing config.ini in " << fs::path(editor).filename().string() << "\n";
	}

	void
	print_config_install_error(const howdy::native::config_internal::ConfigEditResult &result) {
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

auto howdy::native::config_internal::config_main_with_dependencies(
    int argc, char **argv, const ConfigDependencies &dependencies) -> int {
	if (argc != 1) {
		std::cout << "Invalid arguments for config\n";
		return kExitAbort;
	}
	(void)argv;
	if (!config_edit_dependencies_available(dependencies)) {
		return kExitAbort;
	}

	const ConfigEditSession session(dependencies);
	const auto result = session.run(ConfigEditRequest{.editor_ready = print_editor_ready});

	switch (result.status) {
		case ConfigEditStatus::kDependenciesUnavailable:
			return kExitAbort;
		case ConfigEditStatus::kOk:
			std::cout << "Config updated\n";
			return kExitOk;
		case ConfigEditStatus::kNoChanges:
			std::cout << "No config changes made\n";
			return kExitOk;
		case ConfigEditStatus::kEditorUnavailable:
			std::cout << "Error: No suitable text editor found.\n";
			std::cout << "Set EDITOR to an absolute executable path, or install one of: micro, "
			             "nano, vi.\n";
			return kExitAbort;
		case ConfigEditStatus::kSecurityCheckFailed:
			std::cout << result.error << "\n";
			return kExitAbort;
		case ConfigEditStatus::kTempCreateFailed:
			std::cout << "Failed to prepare a temporary config copy\n";
			return kExitAbort;
		case ConfigEditStatus::kEditorLaunchFailed:
			std::cout << "Failed to launch editor\n";
			return kExitAbort;
		case ConfigEditStatus::kEditorFailed:
			std::cout << "Editor exited unsuccessfully; config not updated\n";
			return kExitAbort;
		case ConfigEditStatus::kReadFailed:
			std::cout << howdy::native::kEditedConfigInstallFailedMessage << '\n';
			return kExitAbort;
		case ConfigEditStatus::kInvalidEditedConfig:
		case ConfigEditStatus::kConfigChanged:
		case ConfigEditStatus::kInstallFailed:
			print_config_install_error(result);
			return kExitAbort;
	}

	return kExitAbort;
}

auto config_main(int argc, char **argv) -> int {
	return howdy::native::config_internal::config_main_with_dependencies(
	    argc, argv, howdy::native::config_internal::default_config_edit_dependencies());
}
