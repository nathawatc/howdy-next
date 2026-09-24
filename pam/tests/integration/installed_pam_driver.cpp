#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string_view>

#include <fcntl.h>
#include <unistd.h>

#include <security/pam_appl.h>

namespace {
	auto InformationalConversation(int message_count, const pam_message **messages,
	                               pam_response **responses, void *data) -> int {
		if (message_count <= 0 || messages == nullptr || responses == nullptr || data == nullptr) {
			return PAM_CONV_ERR;
		}
		for (const pam_message *message :
		     std::span(messages, static_cast<std::size_t>(message_count))) {
			if (message == nullptr || message->msg_style != PAM_TEXT_INFO) {
				std::cerr << "Unexpected PAM conversation request\n";
				return PAM_CONV_ERR;
			}
		}
		*responses = static_cast<pam_response *>(
		    std::calloc(static_cast<std::size_t>(message_count), sizeof(pam_response)));
		if (*responses == nullptr) {
			return PAM_BUF_ERR;
		}
		*static_cast<int *>(data) += message_count;
		return PAM_SUCCESS;
	}
}  // namespace

auto main(int argc, char **argv) -> int {
	const auto arguments =
	    std::span(argv, argc > 0 ? static_cast<std::size_t>(argc) : std::size_t{0});
	if (arguments.size() != 5) {
		const char *program = arguments.empty() ? "installed_pam_driver" : arguments.front();
		std::cerr << "Usage: " << program
		          << " <service> <user> <config-directory> <success|unavailable>\n";
		return 2;
	}
	const char            *service          = arguments.subspan(1).front();
	const char            *user             = arguments.subspan(2).front();
	const char            *config_directory = arguments.subspan(3).front();
	const std::string_view expected(arguments.back());
	if (expected != "success" && expected != "unavailable") {
		std::cerr << "Expected success or unavailable\n";
		return 2;
	}

	int            information_count = 0;
	const pam_conv conversation{.conv        = InformationalConversation,
	                            .appdata_ptr = &information_count};
	pam_handle_t  *handle = nullptr;
	const int      start_result =
	    pam_start_confdir(service, user, &conversation, config_directory, &handle);
	if (start_result != PAM_SUCCESS) {
		std::cerr << "pam_start_confdir failed: " << pam_strerror(handle, start_result) << " ("
		          << start_result << ")\n";
		return 1;
	}

	// The compare child must not inherit a descriptor opened by the PAM caller.
	const int source_fd   = open("/dev/null", O_RDONLY | O_CLOEXEC);
	const int sentinel_fd = source_fd < 0 ? -1 : fcntl(source_fd, F_DUPFD, 80);
	if (source_fd >= 0) {
		close(source_fd);
	}
	if (sentinel_fd != 80) {
		std::cerr << "Could not set up inherited-descriptor sentinel\n";
		if (sentinel_fd >= 0) {
			close(sentinel_fd);
		}
		pam_end(handle, PAM_SYSTEM_ERR);
		return 1;
	}
	const int auth_result    = pam_authenticate(handle, 0);
	const int sentinel_flags = fcntl(80, F_GETFD);
	const int end_result     = pam_end(handle, auth_result);
	close(80);
	if (sentinel_flags < 0 ||
	    (static_cast<unsigned int>(sentinel_flags) & static_cast<unsigned int>(FD_CLOEXEC)) != 0) {
		std::cerr << "PAM caller descriptor sentinel was lost\n";
		return 1;
	}
	if (end_result != PAM_SUCCESS) {
		std::cerr << "pam_end failed: " << end_result << "\n";
		return 1;
	}
	const int expected_result = expected == "success" ? PAM_SUCCESS : PAM_AUTHINFO_UNAVAIL;
	if (auth_result != expected_result) {
		std::cerr << "pam_authenticate returned " << auth_result << ", expected " << expected_result
		          << "\n";
		return 1;
	}
	const int expected_information_count = expected == "success" ? 2 : 0;
	if (information_count != expected_information_count) {
		std::cerr << "PAM information messages: " << information_count << ", expected "
		          << expected_information_count << "\n";
		return 1;
	}

	std::cout << (expected == "success" ? "PAM_SUCCESS" : "PAM_AUTHINFO_UNAVAIL") << '\n';
	return 0;
}
