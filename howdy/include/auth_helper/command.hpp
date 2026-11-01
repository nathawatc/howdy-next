#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace howdy::native::auth_helper::command {

	__attribute__((visibility("hidden"))) auto
	PrintPreparedPaths(const std::filesystem::path &config_path,
	                   const std::filesystem::path &user_models_dir) -> void;
	// Sends each line to syslog under a fixed ident; the default ident comes from argv[0], which
	// the caller of the setuid helper controls.
	__attribute__((visibility("hidden"))) void SyslogDiagnosticLines(std::string_view text);
	// PAM reads helper stdout and stderr as one stream and rejects any line it does not expect, so
	// diagnostics written while `operation` runs reach stderr only when it fails or throws, and
	// go to `report_success` when it succeeds.
	__attribute__((visibility("hidden"))) auto RunWithQuietDiagnostics(
	    const std::function<bool()>                 &operation,
	    const std::function<void(std::string_view)> &report_success = SyslogDiagnosticLines)
	    -> bool;
	__attribute__((visibility("hidden"))) auto PrepareForUser(const std::string &user) -> int;
	__attribute__((visibility("hidden"))) auto Run(int argc, char **argv) -> int;

}  // namespace howdy::native::auth_helper::command
