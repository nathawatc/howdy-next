#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

#include <sys/types.h>

namespace howdy::native::user_model_readiness_internal {

	enum class __attribute__((visibility("hidden"))) StagedPathKind : std::uint8_t {
		kCanonical,
		kMalformed,
		kStaged,
	};

	enum class __attribute__((visibility("hidden"))) StagedReadiness : std::uint8_t {
		kPresent,
		kAbsent,
		kInsecure,
		kError,
	};

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	classify_staged_path(const std::filesystem::path &path) -> StagedPathKind;

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	inspect_staged_model(const std::filesystem::path &path, std::optional<uid_t> owner_uid)
	    -> StagedReadiness;

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	validate_staged_model_file(int fd, const std::filesystem::path &path,
	                           std::optional<uid_t> owner_uid) -> bool;

}  // namespace howdy::native::user_model_readiness_internal
