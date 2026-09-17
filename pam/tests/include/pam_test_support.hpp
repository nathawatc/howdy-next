#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include <security/pam_appl.h>

namespace howdy::test {

	class PamTestConfigDirectory {
	public:
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
