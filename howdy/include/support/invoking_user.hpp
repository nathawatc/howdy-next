#pragma once

#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <pwd.h>
#include <string>
#include <string_view>

#include <sys/types.h>

namespace howdy::native {
	inline constexpr auto kSudoUserEnvironmentVariable  = "SUDO_USER";
	inline constexpr auto kSudoUidEnvironmentVariable   = "SUDO_UID";
	inline constexpr auto kSudoGidEnvironmentVariable   = "SUDO_GID";
	inline constexpr auto kDoasUserEnvironmentVariable  = "DOAS_USER";
	inline constexpr auto kPkexecUidEnvironmentVariable = "PKEXEC_UID";

	struct InvokingUser {
		uid_t       uid = 0;
		gid_t       gid = 0;
		std::string name;
		std::string home;
		std::string shell;
	};

	enum class InvokingIdentityStatus : std::uint8_t {
		kNoWrapperIdentity,
		kResolved,
		kInvalid,
		kConflicting,
	};

	struct InvokingIdentityResult {
		InvokingIdentityStatus      status = InvokingIdentityStatus::kNoWrapperIdentity;
		std::optional<InvokingUser> user;
	};

	namespace detail {

		template <typename IdType>
		inline auto ParseIdEnv(const char *value) -> std::optional<IdType> {
			if (value == nullptr || value[0] == '\0') {
				return std::nullopt;
			}

			IdType parsed = 0;
			for (const char *cursor = value; *cursor != '\0'; ++cursor) {
				if (*cursor < '0' || *cursor > '9') {
					return std::nullopt;
				}

				const auto digit = static_cast<IdType>(*cursor - '0');
				if (parsed >
				    (std::numeric_limits<IdType>::max() - digit) / static_cast<IdType>(10)) {
					return std::nullopt;
				}
				parsed = (parsed * static_cast<IdType>(10)) + digit;
			}

			return parsed;
		}

		inline auto HasPasswdName(const passwd &pwd) -> bool {
			return pwd.pw_name != nullptr && pwd.pw_name[0] != '\0';
		}

		inline auto PasswdNameMatches(const passwd &pwd, const char *expected_name) -> bool {
			return expected_name != nullptr && expected_name[0] != '\0' && HasPasswdName(pwd) &&
			       std::string_view(pwd.pw_name) == expected_name;
		}

	}  // namespace detail

	inline auto ParseUidEnv(const char *value) -> std::optional<uid_t> {
		return detail::ParseIdEnv<uid_t>(value);
	}

	inline auto ParseGidEnv(const char *value) -> std::optional<gid_t> {
		return detail::ParseIdEnv<gid_t>(value);
	}

	inline auto InvokingUserFromPwd(const passwd &pwd) -> InvokingUser {
		return InvokingUser{
		    .uid   = pwd.pw_uid,
		    .gid   = pwd.pw_gid,
		    .name  = pwd.pw_name != nullptr ? pwd.pw_name : "",
		    .home  = pwd.pw_dir != nullptr ? pwd.pw_dir : "",
		    .shell = pwd.pw_shell != nullptr ? pwd.pw_shell : "",
		};
	}

	inline auto ResolveInvokingIdentity() -> InvokingIdentityResult {
		const char *sudo_user  = std::getenv(kSudoUserEnvironmentVariable);
		const char *sudo_uid   = std::getenv(kSudoUidEnvironmentVariable);
		const char *sudo_gid   = std::getenv(kSudoGidEnvironmentVariable);
		const char *doas_user  = std::getenv(kDoasUserEnvironmentVariable);
		const char *pkexec_uid = std::getenv(kPkexecUidEnvironmentVariable);

		const bool sudo_present =
		    sudo_user != nullptr || sudo_uid != nullptr || sudo_gid != nullptr;
		const bool doas_present   = doas_user != nullptr;
		const bool pkexec_present = pkexec_uid != nullptr;
		const auto wrapper_count  = static_cast<unsigned>(sudo_present) +
		                            static_cast<unsigned>(doas_present) +
		                            static_cast<unsigned>(pkexec_present);
		if (wrapper_count > 1U) {
			return {.status = InvokingIdentityStatus::kConflicting};
		}

		if (wrapper_count == 0U) {
			return {.status = InvokingIdentityStatus::kNoWrapperIdentity};
		}

		if (sudo_present) {
			const auto parsed_uid = ParseUidEnv(sudo_uid);
			const auto parsed_gid = ParseGidEnv(sudo_gid);
			if (sudo_user == nullptr || !parsed_uid.has_value() || !parsed_gid.has_value()) {
				return {.status = InvokingIdentityStatus::kInvalid};
			}

			passwd *pwd = getpwnam(sudo_user);
			if (pwd == nullptr || !detail::PasswdNameMatches(*pwd, sudo_user) ||
			    pwd->pw_uid != *parsed_uid) {
				return {.status = InvokingIdentityStatus::kInvalid};
			}

			// SUDO_GID is the invoking process's group ID and may differ from the
			// account's passwd primary group, for example after newgrp(1).
			auto invoking_user = InvokingUserFromPwd(*pwd);
			invoking_user.gid  = *parsed_gid;
			return {
			    .status = InvokingIdentityStatus::kResolved,
			    .user   = invoking_user,
			};
		}

		if (doas_present) {
			if (doas_user == nullptr || doas_user[0] == '\0') {
				return {.status = InvokingIdentityStatus::kInvalid};
			}

			passwd *pwd = getpwnam(doas_user);
			if (pwd == nullptr || !detail::PasswdNameMatches(*pwd, doas_user)) {
				return {.status = InvokingIdentityStatus::kInvalid};
			}
			return {
			    .status = InvokingIdentityStatus::kResolved,
			    .user   = InvokingUserFromPwd(*pwd),
			};
		}

		const auto parsed_uid = ParseUidEnv(pkexec_uid);
		if (!parsed_uid.has_value()) {
			return {.status = InvokingIdentityStatus::kInvalid};
		}
		passwd *pwd = getpwuid(*parsed_uid);
		if (pwd == nullptr || !detail::HasPasswdName(*pwd)) {
			return {.status = InvokingIdentityStatus::kInvalid};
		}
		return {
		    .status = InvokingIdentityStatus::kResolved,
		    .user   = InvokingUserFromPwd(*pwd),
		};
	}

}  // namespace howdy::native
