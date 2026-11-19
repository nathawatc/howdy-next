#pragma once

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>

#include <sys/types.h>

namespace howdy::native::auth_helper_protocol {

	inline constexpr const char *kConfigPathKey    = "CONFIG_PATH";
	inline constexpr const char *kUserModelsDirKey = "USER_MODELS_DIR";
	// After a successful prepare, once the lease was sent and the paths were written, the helper
	// sends this one-byte message, with no descriptors, as the lease socket's last message. A
	// current PAM module requires it on every host, and it stands in for the exit status when
	// the PAM application reaped the helper and took that status (for example SIGCHLD set to
	// SIG_IGN); a socket, unlike the stdout pipe, cannot be
	// reopened through /proc/<pid>/fd to add one. The helper sends it only when PAM asks with
	// kCompletionRequestName=kCompletionRequestValue in its environment: a pam_howdy.so loaded
	// before an upgrade never asks and still expects the lease socket to end right after the
	// lease.
	inline constexpr char        kCompletionMarker       = 'C';
	inline constexpr const char *kCompletionRequestName  = "HOWDY_AUTH_HELPER_COMPLETION";
	inline constexpr const char *kCompletionRequestValue = "1";
	inline constexpr const char *kCompletionRequestEntry = "HOWDY_AUTH_HELPER_COMPLETION=1";

	// PAM sends the entry, the helper checks name and value; keep them in step.
	static_assert(std::string_view(kCompletionRequestEntry).starts_with(kCompletionRequestName) &&
	              std::string_view(kCompletionRequestEntry).ends_with(kCompletionRequestValue) &&
	              std::string_view(kCompletionRequestEntry).size() ==
	                  std::string_view(kCompletionRequestName).size() + 1 +
	                      std::string_view(kCompletionRequestValue).size() &&
	              std::string_view(kCompletionRequestEntry)
	                      .at(std::string_view(kCompletionRequestName).size()) == '=');

	inline constexpr const char *kPreparedRuntimeRoot             = "/run/howdy";
	inline constexpr const char *kPreparedRuntimeDirectoryPrefix  = "pam-";
	inline constexpr const char *kPreparedConfigFileName          = "config.ini";
	inline constexpr const char *kPreparedUserModelsDirectoryName = "models";
	inline constexpr const char *kPreparedModelBackingFileName    = ".model_backing";
	inline constexpr int         kLeaseSocketFd                   = 3;

	enum class RuntimeGenerationSlot : std::uint8_t {
		kSlot0,
		kSlot1,
	};

	inline auto PreparedRuntimeRoot() -> std::filesystem::path {
		return kPreparedRuntimeRoot;
	}

	inline auto PreparedRuntimeDirectoryPrefix(uid_t uid) -> std::string {
		return std::string(kPreparedRuntimeDirectoryPrefix) + std::to_string(uid) + "-";
	}

	inline auto RuntimeGenerationSuffix(RuntimeGenerationSlot slot) -> std::string_view {
		return slot == RuntimeGenerationSlot::kSlot0 ? "gen000" : "gen001";
	}

	inline auto PreparedRuntimeGenerationName(uid_t uid, RuntimeGenerationSlot slot)
	    -> std::string {
		return PreparedRuntimeDirectoryPrefix(uid) + std::string(RuntimeGenerationSuffix(slot));
	}

	inline auto PreparedRuntimeGenerationDir(const std::filesystem::path &root, uid_t uid,
	                                         RuntimeGenerationSlot slot) -> std::filesystem::path {
		return root / PreparedRuntimeGenerationName(uid, slot);
	}

	inline auto PreparedRuntimeGenerationLockName(uid_t uid, RuntimeGenerationSlot slot)
	    -> std::string {
		return PreparedRuntimeGenerationName(uid, slot) + ".lock";
	}

	inline auto PreparedRuntimeGenerationLockPath(const std::filesystem::path &root, uid_t uid,
	                                              RuntimeGenerationSlot slot)
	    -> std::filesystem::path {
		return root / PreparedRuntimeGenerationLockName(uid, slot);
	}

	inline auto PreparedRuntimeGenerationLockPath(const std::filesystem::path &runtime_dir)
	    -> std::filesystem::path {
		return {runtime_dir.string() + ".lock"};
	}

	// Side directory a slot is rebuilt in before it is swapped into place.
	inline auto PreparedRuntimeGenerationBuildPath(const std::filesystem::path &runtime_dir)
	    -> std::filesystem::path {
		return runtime_dir.parent_path() / ("." + runtime_dir.filename().string() + ".build");
	}

	inline auto PreparedConfigPath(const std::filesystem::path &runtime_dir)
	    -> std::filesystem::path {
		return runtime_dir / kPreparedConfigFileName;
	}

	inline auto PreparedUserModelsDir(const std::filesystem::path &runtime_dir)
	    -> std::filesystem::path {
		return runtime_dir / kPreparedUserModelsDirectoryName;
	}

	inline auto IsCanonicalAbsolutePath(const std::filesystem::path &path) -> bool {
		return path.is_absolute() && path.string() == path.lexically_normal().string();
	}

	inline auto ParseRuntimeGenerationName(std::string_view name, uid_t *uid,
	                                       RuntimeGenerationSlot *slot) noexcept -> bool {
		constexpr std::string_view prefix = kPreparedRuntimeDirectoryPrefix;
		if (uid == nullptr || slot == nullptr || !name.starts_with(prefix)) {
			return false;
		}

		const auto suffix_position = name.rfind("-gen");
		if (suffix_position == std::string_view::npos || suffix_position <= prefix.size()) {
			return false;
		}
		const auto suffix = name.substr(suffix_position + 1);
		if (suffix == "gen000") {
			*slot = RuntimeGenerationSlot::kSlot0;
		} else if (suffix == "gen001") {
			*slot = RuntimeGenerationSlot::kSlot1;
		} else {
			return false;
		}

		const auto uid_text = name.substr(prefix.size(), suffix_position - prefix.size());
		if (uid_text.empty() || (uid_text.size() > 1 && uid_text.front() == '0')) {
			return false;
		}
		std::uintmax_t value    = 0;
		const auto [end, error] = std::from_chars(uid_text.begin(), uid_text.end(), value, 10);
		if (error != std::errc{} || end != uid_text.end() ||
		    value > std::numeric_limits<uid_t>::max()) {
			return false;
		}
		*uid = static_cast<uid_t>(value);
		return true;
	}

	// `runtime_root` must itself be canonical and absolute; anything else fails closed.
	inline auto MatchesPreparedRuntimeLayout(const std::filesystem::path &runtime_root,
	                                         const std::filesystem::path &runtime_dir,
	                                         const std::filesystem::path &config_path,
	                                         const std::filesystem::path &user_models_dir,
	                                         uid_t                        expected_uid) -> bool {
		if (!IsCanonicalAbsolutePath(runtime_root) || !IsCanonicalAbsolutePath(runtime_dir) ||
		    !IsCanonicalAbsolutePath(config_path) || !IsCanonicalAbsolutePath(user_models_dir) ||
		    runtime_dir.parent_path() != runtime_root) {
			return false;
		}

		uid_t                 parsed_uid = 0;
		RuntimeGenerationSlot slot{};
		if (!ParseRuntimeGenerationName(runtime_dir.filename().string(), &parsed_uid, &slot) ||
		    parsed_uid != expected_uid) {
			return false;
		}
		return config_path == PreparedConfigPath(runtime_dir) &&
		       user_models_dir == PreparedUserModelsDir(runtime_dir);
	}

	inline auto MatchesPreparedRuntimeLayout(const std::filesystem::path &runtime_dir,
	                                         const std::filesystem::path &config_path,
	                                         const std::filesystem::path &user_models_dir,
	                                         uid_t                        expected_uid) -> bool {
		return MatchesPreparedRuntimeLayout(PreparedRuntimeRoot(), runtime_dir, config_path,
		                                    user_models_dir, expected_uid);
	}

}  // namespace howdy::native::auth_helper_protocol
