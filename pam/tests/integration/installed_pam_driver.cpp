#include <cstddef>
#include <iostream>
#include <span>

#include <security/pam_appl.h>

namespace {
	auto RejectConversation(int message_count, const pam_message **messages,
	                        pam_response **responses, void *data) -> int {
		(void)message_count;
		(void)messages;
		(void)responses;
		(void)data;
		std::cerr << "Unexpected PAM conversation request\n";
		return PAM_CONV_ERR;
	}
}  // namespace

auto main(int argc, char **argv) -> int {
	const auto arguments =
	    std::span(argv, argc > 0 ? static_cast<std::size_t>(argc) : std::size_t{0});
	if (arguments.size() != 4) {
		const char *program = arguments.empty() ? "installed_pam_driver" : arguments.front();
		std::cerr << "Usage: " << program << " <service> <user> <config-directory>\n";
		return 2;
	}
	const char *service          = arguments.subspan(1).front();
	const char *user             = arguments.subspan(2).front();
	const char *config_directory = arguments.back();

	pam_conv      conversation{.conv = RejectConversation, .appdata_ptr = nullptr};
	pam_handle_t *handle = nullptr;
	const int     start_result =
	    pam_start_confdir(service, user, &conversation, config_directory, &handle);
	if (start_result != PAM_SUCCESS) {
		std::cerr << "pam_start_confdir failed: " << pam_strerror(handle, start_result) << " ("
		          << start_result << ")\n";
		return 1;
	}

	const int auth_result = pam_authenticate(handle, 0);
	const int end_result  = pam_end(handle, auth_result);
	if (end_result != PAM_SUCCESS) {
		std::cerr << "pam_end failed: " << end_result << "\n";
		return 1;
	}
	if (auth_result != PAM_AUTHINFO_UNAVAIL) {
		std::cerr << "pam_authenticate returned " << auth_result << ", expected "
		          << PAM_AUTHINFO_UNAVAIL << "\n";
		return 1;
	}

	std::cout << "PAM_AUTHINFO_UNAVAIL\n";
	return 0;
}
