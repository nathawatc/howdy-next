#pragma once

#include "auth_helper/acl.hpp"
#include "auth_helper/runtime.hpp"
#include "auth_helper/runtime/internal.hpp"

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>

namespace howdy::native::auth_helper::runtime_internal {

	using UniqueFd = howdy::native::ScopedFd;

	struct __attribute__((visibility("hidden"))) SourceFile {
		UniqueFd    fd;
		struct stat initial_stat{};
	};

	struct __attribute__((visibility("hidden"))) Slot {
		std::filesystem::path path;
		UniqueFd              lock_fd;
		UniqueFd              dir_fd;
	};

	using SlotSet = std::array<std::optional<Slot>, 2>;

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	SourceUnchanged(const SourceFile &source) -> bool;

	[[nodiscard]] __attribute__((visibility("hidden"))) auto CompareFiles(int left_fd, int right_fd)
	    -> bool;

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	CopySourceToOpenFile(const SourceFile &source, int destination_fd) -> bool;

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	OpenSourceFile(const std::filesystem::path &path, const std::string &label, uid_t owner_uid)
	    -> std::optional<SourceFile>;

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	OpenOrCreateRoot(const std::filesystem::path &path, uid_t owner_uid, gid_t owner_gid)
	    -> std::optional<UniqueFd>;

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	OpenRootOnlyLock(int root_fd, const std::string &name,
	                 const std::filesystem::path &display_path, internal::StagedIdentity identity,
	                 const AclOperations &operations, bool create) -> std::optional<UniqueFd>;

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	OpenSlots(int root_fd, const internal::RuntimeSources &sources,
	          internal::StagedIdentity identity, const AclOperations &operations)
	    -> std::optional<SlotSet>;

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	LeaseFreshSlot(SlotSet &slots, const SourceFile &config_source,
	               const std::optional<SourceFile> &model_source, const std::string &user,
	               internal::StagedIdentity identity, const AclOperations &operations)
	    -> std::optional<PreparedPaths>;

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	RefreshAvailableSlot(SlotSet &slots, const SourceFile &config_source,
	                     const std::optional<SourceFile> &model_source, const std::string &user,
	                     internal::StagedIdentity identity, const AclOperations &operations)
	    -> std::optional<PreparedPaths>;

}  // namespace howdy::native::auth_helper::runtime_internal
