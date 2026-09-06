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

	inline constexpr const char      *kPreparedRuntimeRoot                    = "/run/howdy";
	inline constexpr const char      *kPreparedRuntimeDirectoryPrefix         = "pam-";
	inline constexpr std::string_view kPreparedRuntimeDirectorySuffixTemplate = "XXXXXX";
	inline constexpr auto             kPreparedRuntimeDirectorySuffixLength =
	    kPreparedRuntimeDirectorySuffixTemplate.size();
	inline constexpr const char *kPreparedConfigFileName          = "config.ini";
	inline constexpr const char *kPreparedUserModelsDirectoryName = "models";
	inline constexpr const char *kPreparedModelBackingFileName    = ".model_backing";
	inline constexpr int         kLeaseSocketFd                   = 3;

	enum class RuntimeGenerationSlot : std::uint8_t {
		kSlot0,
		kSlot1,
	};

	inline auto prepared_runtime_root() -> std::filesystem::path {
		return kPreparedRuntimeRoot;
	}

	inline auto prepared_runtime_directory_prefix(uid_t uid) -> std::string {
		return std::string(kPreparedRuntimeDirectoryPrefix) + std::to_string(uid) + "-";
	}

	inline auto prepared_runtime_directory_template(uid_t uid) -> std::string {
		return prepared_runtime_directory_prefix(uid) +
		       std::string(kPreparedRuntimeDirectorySuffixTemplate);
	}

	inline auto runtime_generation_suffix(RuntimeGenerationSlot slot) -> std::string_view {
		return slot == RuntimeGenerationSlot::kSlot0 ? "gen000" : "gen001";
	}

	inline auto prepared_runtime_generation_name(uid_t uid, RuntimeGenerationSlot slot)
	    -> std::string {
		return prepared_runtime_directory_prefix(uid) +
		       std::string(runtime_generation_suffix(slot));
	}

	inline auto prepared_runtime_generation_dir(const std::filesystem::path &root, uid_t uid,
	                                            RuntimeGenerationSlot slot)
	    -> std::filesystem::path {
		return root / prepared_runtime_generation_name(uid, slot);
	}

	inline auto prepared_runtime_generation_lock_name(uid_t uid, RuntimeGenerationSlot slot)
	    -> std::string {
		return prepared_runtime_generation_name(uid, slot) + ".lock";
	}

	inline auto prepared_runtime_generation_lock_path(const std::filesystem::path &root, uid_t uid,
	                                                  RuntimeGenerationSlot slot)
	    -> std::filesystem::path {
		return root / prepared_runtime_generation_lock_name(uid, slot);
	}

	inline auto prepared_runtime_generation_lock_path(const std::filesystem::path &runtime_dir)
	    -> std::filesystem::path {
		return {runtime_dir.string() + ".lock"};
	}

	inline auto prepared_config_path(const std::filesystem::path &runtime_dir)
	    -> std::filesystem::path {
		return runtime_dir / kPreparedConfigFileName;
	}

	inline auto prepared_user_models_dir(const std::filesystem::path &runtime_dir)
	    -> std::filesystem::path {
		return runtime_dir / kPreparedUserModelsDirectoryName;
	}

	inline auto is_canonical_absolute_path(const std::filesystem::path &path) -> bool {
		return path.is_absolute() && path.string() == path.lexically_normal().string();
	}

	inline auto parse_runtime_generation_name(std::string_view name, uid_t *uid,
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
		std::uintmax_t value = 0;
		const auto [end, error] =
		    std::from_chars(uid_text.data(), uid_text.data() + uid_text.size(), value, 10);
		if (error != std::errc{} || end != uid_text.data() + uid_text.size() ||
		    value > std::numeric_limits<uid_t>::max()) {
			return false;
		}
		*uid = static_cast<uid_t>(value);
		return true;
	}

	inline auto matches_prepared_runtime_layout(const std::filesystem::path &runtime_dir,
	                                            const std::filesystem::path &config_path,
	                                            const std::filesystem::path &user_models_dir,
	                                            uid_t                        expected_uid) -> bool {
		if (!is_canonical_absolute_path(runtime_dir) || !is_canonical_absolute_path(config_path) ||
		    !is_canonical_absolute_path(user_models_dir) ||
		    runtime_dir.parent_path() != prepared_runtime_root()) {
			return false;
		}

		uid_t                 parsed_uid = 0;
		RuntimeGenerationSlot slot{};
		if (!parse_runtime_generation_name(runtime_dir.filename().string(), &parsed_uid, &slot) ||
		    parsed_uid != expected_uid) {
			return false;
		}
		return config_path == prepared_config_path(runtime_dir) &&
		       user_models_dir == prepared_user_models_dir(runtime_dir);
	}

}  // namespace howdy::native::auth_helper_protocol
