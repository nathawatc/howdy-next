#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

#include <security/pam_appl.h>

struct pam_handle {};

namespace howdy::test {

	inline auto OpaquePamHandleForTest() noexcept -> pam_handle_t * {
		static pam_handle handle{};
		return &handle;
	}

	inline auto PamResponseSentinelForTest() noexcept -> pam_response * {
		static thread_local pam_response sentinel{};
		return &sentinel;
	}

	class PamTestConfigDirectory {
	public:
		PamTestConfigDirectory(const PamTestConfigDirectory &)                     = delete;
		auto operator=(const PamTestConfigDirectory &) -> PamTestConfigDirectory & = delete;
		PamTestConfigDirectory(PamTestConfigDirectory &&)                          = delete;
		auto operator=(PamTestConfigDirectory &&) -> PamTestConfigDirectory &      = delete;

		PamTestConfigDirectory()
		    : root_(std::filesystem::temp_directory_path() /
		            ("howdy-pam-test-" + std::to_string(getpid()))) {
			std::error_code error;
			std::filesystem::remove_all(root_, error);
			valid_ = std::filesystem::create_directories(root_, error) && !error;
		}

		~PamTestConfigDirectory() {
			std::error_code error;
			std::filesystem::remove_all(root_, error);
		}

		auto Start(const char *service, const char *username, const struct pam_conv *conversation,
		           pam_handle_t **handle) -> int {
#ifdef HOWDY_HAVE_PAM_START_CONFDIR
			if (!valid_) {
				return PAM_SYSTEM_ERR;
			}
			std::ofstream config(root_ / service, std::ios::trunc);
			config << "# isolated Howdy PAM test service\n";
			if (!config.good()) {
				return PAM_SYSTEM_ERR;
			}
			config.close();
			return pam_start_confdir(service, username, conversation, root_.c_str(), handle);
#else
			return pam_start(service, username, conversation, handle);
#endif
		}

	private:
		std::filesystem::path root_;
		bool                  valid_ = false;
	};

	inline auto PamStartForTest(const char *service, const char *username,
	                            const struct pam_conv *conversation, pam_handle_t **handle) -> int {
		static PamTestConfigDirectory config;
		return config.Start(service, username, conversation, handle);
	}

}  // namespace howdy::test
