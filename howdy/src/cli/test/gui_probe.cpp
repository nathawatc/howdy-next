#include "cli/test/gui_probe.hpp"

#include "support/error_message.hpp"
#include "support/invoking_user.hpp"

#include <cerrno>
#include <cstddef>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <sys/types.h>

namespace {

	using howdy::native::test_cli_internal::GuiProbeOperations;

	auto ErrnoMessage(std::string_view step) -> std::string {
		const int err = errno;
		return std::string(step) + " failed: " + howdy::native::SystemErrorMessageWithNumber(err);
	}

	auto RestoreRootCredentials(const GuiProbeOperations &operations,
	                            const std::vector<gid_t> &groups, const gid_t egid)
	    -> std::optional<std::string> {
		// seteuid(0) must come first: setegid/setgroups need the root effective UID back.
		if (operations.seteuid(operations.context, 0) != 0) {
			return ErrnoMessage("Restoring root effective UID after GUI probe");
		}
		if (operations.setegid(operations.context, egid) != 0) {
			return ErrnoMessage("Restoring root effective GID after GUI probe");
		}
		if (operations.setgroups(operations.context, groups.size(), groups.data()) != 0) {
			return ErrnoMessage("Restoring root supplementary groups after GUI probe");
		}
		if (operations.geteuid(operations.context) != 0 ||
		    operations.getegid(operations.context) != egid) {
			return "Root credentials did not verify after GUI probe";
		}
		return std::nullopt;
	}

	auto SaveGroups(const GuiProbeOperations &operations, std::vector<gid_t> *groups)
	    -> std::optional<std::string> {
		const int group_count = operations.getgroups(operations.context, 0, nullptr);
		if (group_count < 0) {
			return ErrnoMessage("Reading root supplementary groups");
		}
		groups->resize(static_cast<std::size_t>(group_count));
		if (group_count == 0) {
			return std::nullopt;
		}
		const int read_count =
		    operations.getgroups(operations.context, group_count, groups->data());
		// A list that grew since the size query makes the second call fail with EINVAL.
		if (read_count < 0 && errno != EINVAL) {
			return ErrnoMessage("Reading root supplementary groups");
		}
		if (read_count != group_count) {
			return "Root supplementary groups changed while saving them for the GUI probe";
		}
		return std::nullopt;
	}

	auto SwitchToUser(const GuiProbeOperations &operations, const howdy::native::InvokingUser &user)
	    -> std::optional<std::string> {
		// seteuid must come last: initgroups/setegid need the root effective UID.
		if (operations.initgroups(operations.context, user.name.c_str(), user.gid) != 0) {
			return ErrnoMessage("Loading invoking user groups for GUI probe");
		}
		if (operations.setegid(operations.context, user.gid) != 0) {
			return ErrnoMessage("Switching to invoking user GID for GUI probe");
		}
		if (operations.seteuid(operations.context, user.uid) != 0) {
			return ErrnoMessage("Switching to invoking user UID for GUI probe");
		}
		if (operations.geteuid(operations.context) != user.uid ||
		    operations.getegid(operations.context) != user.gid) {
			return "Invoking user credentials did not verify for GUI probe";
		}
		return std::nullopt;
	}

}  // namespace

namespace howdy::native::test_cli_internal {

	auto RunGuiProbeAsInvokingUser(const InvokingUser &user, const GuiProbeOperations &operations)
	    -> std::optional<std::string> {
		if (operations.getgroups == nullptr || operations.setgroups == nullptr ||
		    operations.initgroups == nullptr || operations.getegid == nullptr ||
		    operations.geteuid == nullptr || operations.setegid == nullptr ||
		    operations.seteuid == nullptr || operations.probe == nullptr) {
			return "Internal error: missing GUI probe dependency";
		}

		std::vector<gid_t> saved_groups;
		if (auto save_error = SaveGroups(operations, &saved_groups)) {
			return save_error;
		}
		const gid_t saved_egid = operations.getegid(operations.context);

		if (auto switch_error = SwitchToUser(operations, user)) {
			// Never fall back to probing as root.
			if (const auto restore_error =
			        RestoreRootCredentials(operations, saved_groups, saved_egid)) {
				return *switch_error + "; " + *restore_error;
			}
			return switch_error;
		}
		try {
			operations.probe(operations.context, user);
		} catch (...) {
			// The exception carries no credential context, so surface a failed restore here
			// before it propagates with the probe identity possibly still in effect.
			if (const auto restore_error =
			        RestoreRootCredentials(operations, saved_groups, saved_egid)) {
				std::cerr << *restore_error << "\n";
			}
			throw;
		}
		return RestoreRootCredentials(operations, saved_groups, saved_egid);
	}

}  // namespace howdy::native::test_cli_internal
