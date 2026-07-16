#include <iostream>

#include <security/pam_appl.h>

namespace {
	auto reject_conversation(int message_count, const pam_message **messages,
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
	if (argc != 4) {
		std::cerr << "Usage: " << argv[0] << " <service> <user> <config-directory>\n";
		return 2;
	}

	pam_conv      conversation{.conv = reject_conversation, .appdata_ptr = nullptr};
	pam_handle_t *handle   = nullptr;
	const int start_result = pam_start_confdir(argv[1], argv[2], &conversation, argv[3], &handle);
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
