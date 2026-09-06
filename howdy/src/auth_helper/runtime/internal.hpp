#pragma once

#include "auth_helper/acl.hpp"
#include "auth_helper/runtime.hpp"
#include "auth_helper/runtime/internal.hpp"

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <unistd.h>
#include <utility>

#include <sys/stat.h>
#include <sys/types.h>

namespace howdy::native::auth_helper::runtime_internal {

	class __attribute__((visibility("hidden"))) UniqueFd {
	public:
		UniqueFd() = default;

		explicit UniqueFd(int fd)
		    : fd_(fd) {}

		~UniqueFd() {
			reset();
		}

		UniqueFd(const UniqueFd &)                     = delete;
		auto operator=(const UniqueFd &) -> UniqueFd & = delete;

		UniqueFd(UniqueFd &&other) noexcept
		    : fd_(std::exchange(other.fd_, -1)) {}

		auto operator=(UniqueFd &&other) noexcept -> UniqueFd & {
			if (this != &other) {
				reset(std::exchange(other.fd_, -1));
			}
			return *this;
		}

		[[nodiscard]] auto get() const -> int {
			return fd_;
		}

		[[nodiscard]] auto release() -> int {
			return std::exchange(fd_, -1);
		}

		void reset(int fd = -1) {
			if (fd_ >= 0) {
				(void)close(fd_);
			}
			fd_ = fd;
		}

	private:
		int fd_ = -1;
	};

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
	source_unchanged(const SourceFile &source) -> bool;

	[[nodiscard]] __attribute__((visibility("hidden"))) auto compare_files(int left_fd,
	                                                                       int right_fd) -> bool;

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	copy_source_to_open_file(const SourceFile &source, int destination_fd) -> bool;

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	open_source_file(const std::filesystem::path &path, const std::string &label, uid_t owner_uid)
	    -> std::optional<SourceFile>;

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	open_or_create_root(const std::filesystem::path &path, uid_t owner_uid, gid_t owner_gid)
	    -> std::optional<UniqueFd>;

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	open_root_only_lock(int root_fd, const std::string &name,
	                    const std::filesystem::path &display_path,
	                    internal::StagedIdentity identity, const AclOperations &operations,
	                    bool create) -> std::optional<UniqueFd>;

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	open_slots(int root_fd, const internal::RuntimeSources &sources,
	           internal::StagedIdentity identity, const AclOperations &operations)
	    -> std::optional<SlotSet>;

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	lease_fresh_slot(SlotSet &slots, const SourceFile &config_source,
	                 const std::optional<SourceFile> &model_source, const std::string &user,
	                 internal::StagedIdentity identity, const AclOperations &operations)
	    -> std::optional<PreparedPaths>;

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	refresh_available_slot(SlotSet &slots, const SourceFile &config_source,
	                       const std::optional<SourceFile> &model_source, const std::string &user,
	                       internal::StagedIdentity identity, const AclOperations &operations)
	    -> std::optional<PreparedPaths>;

}  // namespace howdy::native::auth_helper::runtime_internal
