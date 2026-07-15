#include "common/compare_exit.hpp"
#include "status_mapping.hpp"
#include "test_support.hpp"
#include "translation.hpp"

#include <array>
#include <clocale>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <libintl.h>
#include <string>

#include <security/pam_modules.h>

#include <sys/wait.h>

namespace {

	using howdy::test::expect;

	auto make_status(howdy::native::CompareExit exit_code) -> int {
		return static_cast<int>(exit_code) << 8;
	}

}  // namespace

auto main() -> int {
	bool ok = true;

	const char       *initial_locale_ptr  = std::setlocale(LC_ALL, nullptr);
	const std::string initial_locale      = initial_locale_ptr == nullptr ? "" : initial_locale_ptr;
	const char       *initial_domain_ptr  = textdomain(nullptr);
	const std::string initial_domain      = initial_domain_ptr == nullptr ? "" : initial_domain_ptr;
	const char       *initial_binding_ptr = bindtextdomain(GETTEXT_PACKAGE, nullptr);
	const std::string initial_binding = initial_binding_ptr == nullptr ? "" : initial_binding_ptr;
	const char       *initial_language_ptr = std::getenv("LANGUAGE");
	const bool        had_initial_language = initial_language_ptr != nullptr;
	const std::string initial_language     = had_initial_language ? initial_language_ptr : "";

	std::setlocale(LC_ALL, "C");
	textdomain("pam-host-test-domain");
	bindtextdomain(GETTEXT_PACKAGE, HOWDY_TEST_LOCALEDIR);
	const std::string host_domain = textdomain(nullptr);

	ok &= expect(std::string(howdy::pam::translate("Missing Howdy translation")) ==
	                 "Missing Howdy translation",
	             "missing Howdy translation returns source string");
	ok &= expect(std::string(textdomain(nullptr)) == host_domain,
	             "explicit Howdy translation ignores host default domain");

	{
		const auto decision =
		    map_compare_wait_status(make_status(howdy::native::CompareExit::kSuccess));
		ok &= expect(decision.pam_result == PAM_SUCCESS, "success returns PAM_SUCCESS");
		ok &= expect(decision.conversation_kind == ConversationKind::None,
		             "success has no conversation");
		ok &= expect(decision.log_message == "Login approved", "success log message");
	}

	{
		const auto decision =
		    map_compare_wait_status(make_status(howdy::native::CompareExit::kNoFaceModel));
		ok &= expect(decision.pam_result == PAM_AUTH_ERR, "no-model returns PAM_AUTH_ERR");
		ok &= expect(decision.conversation_kind == ConversationKind::None,
		             "no-model has no conversation");
		ok &=
		    expect(decision.log_message == "Failure, no face model known", "no-model log message");
	}

	{
		const auto decision =
		    map_compare_wait_status(make_status(howdy::native::CompareExit::kTimeoutReached));
		ok &= expect(decision.conversation_kind == ConversationKind::Error,
		             "timeout returns error conversation");
		ok &=
		    expect(decision.conversation_message == "Failure, timeout reached", "timeout message");
	}

	{
		const auto decision =
		    map_compare_wait_status(make_status(howdy::native::CompareExit::kAbort));
		ok &= expect(decision.conversation_kind == ConversationKind::None,
		             "abort has no conversation");
		ok &= expect(decision.log_message == "Failure, general abort", "abort log message");
	}

	{
		const auto decision =
		    map_compare_wait_status(make_status(howdy::native::CompareExit::kTooDark));
		ok &= expect(decision.conversation_kind == ConversationKind::Error,
		             "too-dark returns error conversation");
		ok &= expect(decision.conversation_message == "Face detection image too dark",
		             "too-dark message");
	}

	{
		const auto decision =
		    map_compare_wait_status(make_status(howdy::native::CompareExit::kInvalidDevice));
		ok &= expect(decision.pam_result == PAM_AUTH_ERR, "invalid-device fails closed");
		ok &= expect(decision.conversation_kind == ConversationKind::None,
		             "invalid-device has no conversation");
		ok &= expect(decision.log_message ==
		                 "Failure, not possible to open camera at configured path",
		             "invalid-device log message");
	}

	{
		const auto decision = map_compare_wait_status(99 << 8);
		ok &= expect(decision.pam_result == PAM_AUTH_ERR, "unknown exit fails closed");
		ok &= expect(decision.conversation_kind == ConversationKind::Error,
		             "unknown exit returns error conversation");
		ok &= expect(decision.conversation_message == "Unknown error: 99", "unknown exit message");
		ok &= expect(decision.log_message == "Failure, unknown error", "unknown exit log");
	}

	{
		const auto decision = map_compare_wait_status(SIGTERM);
		ok &= expect(decision.pam_result == PAM_AUTH_ERR, "signal exit fails closed");
		ok &= expect(decision.conversation_kind == ConversationKind::None,
		             "signal exit has no conversation");
		ok &= expect(decision.log_message.starts_with("Child killed by signal"), "signal exit log");
	}

	{
		const auto decision = map_compare_wait_status(W_STOPCODE(SIGSTOP));
		ok &= expect(decision.pam_result == PAM_AUTH_ERR, "stopped status fails closed");
		ok &= expect(decision.conversation_kind == ConversationKind::None,
		             "stopped status has no conversation");
		ok &= expect(decision.log_message.empty(), "stopped status has no misleading log");
	}

	{
		const auto decision = map_compare_wait_status(127 << 8);
		ok &= expect(decision.pam_result == PAM_AUTH_ERR,
		             "helper execution-style failure fails closed");
		ok &= expect(decision.conversation_kind == ConversationKind::Error,
		             "helper execution-style failure reports controlled error");
	}

	ok &= expect(build_confirmation_message("alice") == "Identified face as alice",
	             "confirmation message is formatted");
	ok &= expect(build_unknown_error_message(42) == "Unknown error: 42",
	             "unknown error message is formatted");
	ok &= expect(std::string(textdomain(nullptr)) == host_domain,
	             "Howdy message mapping preserves host default domain");

	setenv("LANGUAGE", "th", 1);
	const std::array<const char *, 3> thai_locales         = {"th_TH.UTF-8", "th_TH.utf8", "th_TH"};
	const char                       *selected_thai_locale = nullptr;
	for (const char *candidate : thai_locales) {
		if (std::setlocale(LC_ALL, candidate) != nullptr) {
			selected_thai_locale = candidate;
			break;
		}
	}
	if (selected_thai_locale != nullptr) {
		const auto translated =
		    map_compare_wait_status(make_status(howdy::native::CompareExit::kTimeoutReached));
		ok &= expect(translated.conversation_message == "ยืนยันตัวตนล้มเหลวเนื่องจากหมดเวลา",
		             "Howdy message resolves from explicit Howdy domain");
		ok &= expect(std::string(howdy::pam::translate("Missing Howdy translation")) ==
		                 "Missing Howdy translation",
		             "missing catalog entry returns source string under translated locale");
		ok &= expect(std::string(textdomain(nullptr)) == host_domain,
		             "translated Howdy message ignores host default domain");
	} else {
		std::cerr << "SKIP: Thai locale is not generated; translated catalog assertion not run\n";
	}

	if (had_initial_language) {
		setenv("LANGUAGE", initial_language.c_str(), 1);
	} else {
		unsetenv("LANGUAGE");
	}
	if (!initial_binding.empty()) {
		bindtextdomain(GETTEXT_PACKAGE, initial_binding.c_str());
	}
	if (!initial_domain.empty()) {
		textdomain(initial_domain.c_str());
	}
	if (!initial_locale.empty()) {
		std::setlocale(LC_ALL, initial_locale.c_str());
	}

	if (!ok) {
		return 1;
	}
	return 0;
}
