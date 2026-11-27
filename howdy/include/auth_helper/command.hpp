#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace howdy::native::auth_helper::command {

	// Sends each line to syslog under a fixed ident; the default ident comes from argv[0], which
	// the caller of the setuid helper controls.
	__attribute__((visibility("hidden"))) void SyslogDiagnosticLines(std::string_view text);
	// Diagnostics written while `operation` runs reach stderr only when it fails or throws; PAM
	// points the helper's stderr at /dev/null, so each line also goes to `log` as soon as it is
	// written, which survives PAM killing the helper.
	__attribute__((visibility("hidden"))) auto RunWithQuietDiagnostics(
	    const std::function<bool()>                 &operation,
	    const std::function<void(std::string_view)> &log = SyslogDiagnosticLines) -> bool;
	// Catches every exception from `operation`, logs a diagnostic through `log` and stderr without
	// heap allocation, and returns exit code 1 instead of aborting the helper.
	__attribute__((visibility("hidden"))) auto
	RunGuarded(const std::function<int()>                  &operation,
	           const std::function<void(std::string_view)> &log = SyslogDiagnosticLines) noexcept
	    -> int;
	__attribute__((visibility("hidden"))) auto Run(int argc, char **argv) noexcept -> int;

}  // namespace howdy::native::auth_helper::command
