#pragma once

#include <cstdint>
#include <optional>
#include <utility>

#include <sys/stat.h>

namespace howdy::native {
	enum class StagedRuntimeRole : std::uint8_t {
		kRuntimeRoot,
		kLock,
		kSharedDirectory,
		kConfig,
		kPresentModel,
		kAbsentModel,
	};

	struct StagedAclPermissions {
		bool read    = false;
		bool write   = false;
		bool execute = false;
	};

	struct StagedAclPolicy {
		StagedAclPermissions owner;
		StagedAclPermissions target;
		StagedAclPermissions group;
		StagedAclPermissions mask;
		StagedAclPermissions other;
		bool                 has_named_target = false;
	};

	struct StagedRuntimePolicy {
		mode_t                 mode             = 0;
		std::optional<nlink_t> exact_link_count = std::nullopt;
		StagedAclPolicy        acl;
	};

	inline constexpr StagedAclPermissions kAclRead{.read = true};
	inline constexpr StagedAclPermissions kAclReadWrite{.read = true, .write = true};
	inline constexpr StagedAclPermissions kAclReadExecute{.read = true, .execute = true};
	inline constexpr StagedAclPermissions kAclAll{.read = true, .write = true, .execute = true};

	[[nodiscard]] constexpr auto GetStagedRuntimePolicy(StagedRuntimeRole role)
	    -> StagedRuntimePolicy {
		switch (role) {
			case StagedRuntimeRole::kRuntimeRoot:
				return {.mode = 0711};
			case StagedRuntimeRole::kLock:
			case StagedRuntimeRole::kAbsentModel:
				return {.mode             = 0600,
				        .exact_link_count = 1,
				        .acl              = StagedAclPolicy{.owner = kAclReadWrite}};
			case StagedRuntimeRole::kSharedDirectory:
				return {.mode = 0750,
				        .acl  = StagedAclPolicy{.owner            = kAclAll,
				                                .target           = kAclReadExecute,
				                                .mask             = kAclReadExecute,
				                                .has_named_target = true}};
			case StagedRuntimeRole::kConfig:
				return {.mode             = 0640,
				        .exact_link_count = 1,
				        .acl              = StagedAclPolicy{.owner            = kAclReadWrite,
				                                            .target           = kAclRead,
				                                            .mask             = kAclRead,
				                                            .has_named_target = true}};
			case StagedRuntimeRole::kPresentModel:
				return {.mode             = 0640,
				        .exact_link_count = 2,
				        .acl              = StagedAclPolicy{.owner            = kAclReadWrite,
				                                            .target           = kAclRead,
				                                            .mask             = kAclRead,
				                                            .has_named_target = true}};
		}
		std::unreachable();
	}
}  // namespace howdy::native
