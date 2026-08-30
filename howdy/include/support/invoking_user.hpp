#pragma once

#include <cerrno>
#include <cstdlib>
#include <limits>
#include <optional>
#include <pwd.h>
#include <string>

namespace howdy::native {
	inline constexpr auto kDoasUserEnvironmentVariable  = "DOAS_USER";
	inline constexpr auto kPkexecUidEnvironmentVariable = "PKEXEC_UID";

	struct InvokingUser {
		uid_t       uid = 0;
		gid_t       gid = 0;
		std::string name;
		std::string home;
		std::string shell;
	};

	namespace detail {

		template <typename IdType>
		inline auto parse_id_env(const char *value) -> std::optional<IdType> {
			if (value == nullptr || value[0] == '\0') {
				return std::nullopt;
			}

			errno             = 0;
			char      *end    = nullptr;
			const auto raw_id = std::strtoul(value, &end, 10);
			if (errno != 0 || end == value || end == nullptr || *end != '\0' ||
			    raw_id > std::numeric_limits<IdType>::max()) {
				return std::nullopt;
			}

			return static_cast<IdType>(raw_id);
		}

	}  // namespace detail

	inline auto parse_uid_env(const char *value) -> std::optional<uid_t> {
		return detail::parse_id_env<uid_t>(value);
	}

	inline auto parse_gid_env(const char *value) -> std::optional<gid_t> {
		return detail::parse_id_env<gid_t>(value);
	}

	inline auto invoking_user_from_pwd(const passwd &pwd, gid_t gid_override) -> InvokingUser {
		return InvokingUser{
		    .uid   = pwd.pw_uid,
		    .gid   = gid_override,
		    .name  = pwd.pw_name,
		    .home  = pwd.pw_dir != nullptr ? pwd.pw_dir : "",
		    .shell = pwd.pw_shell != nullptr ? pwd.pw_shell : "",
		};
	}

	inline auto resolve_invoking_user() -> std::optional<InvokingUser> {
		if (const auto sudo_uid = parse_uid_env(std::getenv("SUDO_UID"))) {
			if (passwd *pwd = getpwuid(*sudo_uid); pwd != nullptr) {
				const auto sudo_gid = parse_gid_env(std::getenv("SUDO_GID")).value_or(pwd->pw_gid);
				return invoking_user_from_pwd(*pwd, sudo_gid);
			}
		}

		if (const char *doas_user = std::getenv(kDoasUserEnvironmentVariable);
		    doas_user != nullptr && doas_user[0] != '\0') {
			if (passwd *pwd = getpwnam(doas_user); pwd != nullptr) {
				return invoking_user_from_pwd(*pwd, pwd->pw_gid);
			}
		}

		if (const auto pkexec_uid = parse_uid_env(std::getenv(kPkexecUidEnvironmentVariable))) {
			if (passwd *pwd = getpwuid(*pkexec_uid); pwd != nullptr) {
				return invoking_user_from_pwd(*pwd, pwd->pw_gid);
			}
		}

		return std::nullopt;
	}

}  // namespace howdy::native
