#pragma once

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <pwd.h>

#include <sys/types.h>

namespace howdy::native {

	struct PasswdRecord {
		uid_t       uid = 0;
		gid_t       gid = 0;
		std::string name;
		std::string home;
		std::string shell;
	};

	namespace passwd_lookup_detail {
		constexpr std::size_t kInitialBufferSize = 1024;
		constexpr std::size_t kMaximumBufferSize = std::size_t{1024} * 1024;

		template <typename Lookup> auto LookupPasswd(Lookup lookup) -> std::optional<PasswdRecord> {
			passwd            entry{};
			passwd           *result = nullptr;
			std::vector<char> buffer(kInitialBufferSize);
			while (true) {
				result          = nullptr;
				const int error = lookup(&entry, buffer.data(), buffer.size(), &result);
				if (error == 0) {
					break;
				}
				if (error != ERANGE || buffer.size() >= kMaximumBufferSize) {
					return std::nullopt;
				}
				buffer.resize(std::min(buffer.size() * 2, kMaximumBufferSize));
			}
			if (result == nullptr) {
				return std::nullopt;
			}
			return PasswdRecord{
			    .uid   = result->pw_uid,
			    .gid   = result->pw_gid,
			    .name  = result->pw_name != nullptr ? result->pw_name : "",
			    .home  = result->pw_dir != nullptr ? result->pw_dir : "",
			    .shell = result->pw_shell != nullptr ? result->pw_shell : "",
			};
		}
	}  // namespace passwd_lookup_detail

	inline auto LookupPasswdByName(std::string_view name) -> std::optional<PasswdRecord> {
		if (name.empty() || name.contains('\0')) {
			return std::nullopt;
		}
		const std::string name_storage(name);
		return passwd_lookup_detail::LookupPasswd([&name_storage](passwd *entry, char *buffer,
		                                                          std::size_t buffer_size,
		                                                          passwd    **result) -> int {
			return getpwnam_r(name_storage.c_str(), entry, buffer, buffer_size, result);
		});
	}

	inline auto LookupPasswdByUid(uid_t uid) -> std::optional<PasswdRecord> {
		return passwd_lookup_detail::LookupPasswd(
		    [uid](passwd *entry, char *buffer, std::size_t buffer_size, passwd **result) -> int {
			    return getpwuid_r(uid, entry, buffer, buffer_size, result);
		    });
	}

}  // namespace howdy::native
