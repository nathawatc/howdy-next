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
	// The `open` subcommand: sends the calling user's runtime config and model descriptors as the
	// one message of protocol/runtime_descriptor_message.hpp on descriptor 3.
	__attribute__((visibility("hidden"))) auto OpenForUser(const std::string &user) -> int;
	__attribute__((visibility("hidden"))) auto Run(int argc, char **argv) -> int;

}  // namespace howdy::native::auth_helper::command
