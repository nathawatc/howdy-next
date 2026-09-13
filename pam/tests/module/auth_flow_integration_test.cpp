#include "module/auth_flow_test_support.hpp"
#include "module/entrypoint.hpp"
#include "prompt/prompt_coordinator_fake.hpp"
#include "support/process_test_support.hpp"
#include "test_support.hpp"

#include <array>
#include <chrono>
#include <clocale>
#include <cstdlib>
#include <libintl.h>
#include <optional>
#include <string>
#include <unistd.h>

#include <sys/wait.h>

auto RunAuthFlowIntegrationTests() -> bool;

namespace {
	using namespace howdy::test::auth_flow;
	using namespace howdy::test::process;

	using howdy::pam::RunAuthenticationEntrypoint;
	using howdy::test::expect;

	class ScopedEnv {
	public:
		explicit ScopedEnv(const char *name)
		    : name_(name) {
			const char *value = getenv(name_.c_str());
			if (value != nullptr) {
				original_ = value;
			}
		}

		ScopedEnv(const ScopedEnv &)                     = delete;
		auto operator=(const ScopedEnv &) -> ScopedEnv & = delete;

		~ScopedEnv() {
			if (original_.has_value()) {
				setenv(name_.c_str(), original_->c_str(), 1);
				return;
			}
			unsetenv(name_.c_str());
		}

	private:
		std::string                name_;
		std::optional<std::string> original_;
	};

	auto ExpectAuthenticationNoticeAndConversationGuards() -> bool {
		EligibilityFlowFixture fixture;
		auto                   dependencies = MakeEligibilityFlowDependencies(&fixture);
		ConversationState      state;
		struct pam_conv        conversation{
		    .conv        = TestConversation,
		    .appdata_ptr = &state,
		};
		ScopedPamHandle pam_handle;
		bool            ok = true;
		ok &=
		    expect(pam_handle.Start(&conversation) == PAM_SUCCESS, "notice test starts PAM handle");
		if (pam_handle.Get() == nullptr) {
			return false;
		}

		state.result = PAM_CONV_ERR;
		ok &= expect(RunAuthenticationEntrypoint(pam_handle.Get(), {}, true,
		                                         {.context      = &dependencies,
		                                          .authenticate = IdentifyForTest}) == PAM_SUCCESS,
		             "detection notice conversation failure does not abort authentication");
		ok &= expect(state.calls == 1 && state.last_msg_type == PAM_TEXT_INFO,
		             "enabled detection notice sends text message");

		fixture.runtime.detection_notice = false;
		state.result                     = PAM_SUCCESS;
		state.calls                      = 0;
		ok &= expect(RunAuthenticationEntrypoint(pam_handle.Get(), {}, true,
		                                         {.context      = &dependencies,
		                                          .authenticate = IdentifyForTest}) == PAM_SUCCESS,
		             "disabled detection notice leaves authentication successful");
		ok &= expect(state.calls == 0, "disabled detection notice sends no message");

		const struct pam_conv unavailable{
		    .conv        = nullptr,
		    .appdata_ptr = nullptr,
		};
		ok &= expect(pam_set_item(pam_handle.Get(), PAM_CONV, &unavailable) == PAM_SUCCESS,
		             "installs unavailable PAM conversation");
		ok &=
		    expect(RunAuthenticationEntrypoint(pam_handle.Get(), {}, true,
		                                       {.context      = &dependencies,
		                                        .authenticate = IdentifyForTest}) == PAM_SYSTEM_ERR,
		           "unavailable PAM conversation fails closed");
		return ok;
	}

	auto ExpectRuntimeLoadFailurePaths() -> bool {
		EligibilityFlowFixture fixture;
		auto                   dependencies = MakeEligibilityFlowDependencies(&fixture);
		ConversationState      state;
		struct pam_conv        conversation{
		    .conv        = TestConversation,
		    .appdata_ptr = &state,
		};
		ScopedPamHandle pam_handle;
		bool            ok = true;
		ok &= expect(pam_handle.Start(&conversation) == PAM_SUCCESS,
		             "runtime failure test starts PAM handle");
		if (pam_handle.Get() == nullptr) {
			return false;
		}

		fixture.runtime.initial_load_status = howdy::native::RuntimeConfigLoadStatus::kPathError;
		fixture.runtime.initial_error_code  = EACCES;
		fixture.runtime.effective_uid       = 1000;
		ok &=
		    expect(RunAuthenticationEntrypoint(pam_handle.Get(), {}, true,
		                                       {.context      = &dependencies,
		                                        .authenticate = IdentifyForTest}) == PAM_SYSTEM_ERR,
		           "runtime staging preparation failure maps to PAM_SYSTEM_ERR");
		ok &= expect(fixture.runtime.prepare_calls == 1 && fixture.runtime.load_calls == 1,
		             "runtime staging attempts preparation after inaccessible config");

		fixture.runtime.initial_load_status = howdy::native::RuntimeConfigLoadStatus::kParseError;
		fixture.runtime.initial_error_code  = 0;
		fixture.runtime.effective_uid       = 0;
		fixture.runtime.load_calls          = 0;
		ok &=
		    expect(RunAuthenticationEntrypoint(pam_handle.Get(), {}, true,
		                                       {.context      = &dependencies,
		                                        .authenticate = IdentifyForTest}) == PAM_SYSTEM_ERR,
		           "runtime configuration failure maps to PAM_SYSTEM_ERR");
		ok &= expect(fixture.runtime.prepare_calls == 1 && fixture.runtime.load_calls == 1,
		             "non-staging runtime failure skips preparation");

		fixture.runtime.initial_load_status = howdy::native::RuntimeConfigLoadStatus::kPathError;
		fixture.runtime.initial_error_code  = EACCES;
		fixture.runtime.effective_uid       = 1000;
		fixture.runtime.prepare_result      = true;
		fixture.runtime.staged_load_status  = howdy::native::RuntimeConfigLoadStatus::kOk;
		fixture.runtime.load_calls          = 0;
		ok &= expect(RunAuthenticationEntrypoint(pam_handle.Get(), {}, true,
		                                         {.context      = &dependencies,
		                                          .authenticate = IdentifyForTest}) == PAM_SUCCESS,
		             "successful staged runtime continues authentication");
		ok &= expect(fixture.runtime.prepare_calls == 2 && fixture.runtime.load_calls == 2,
		             "successful staged runtime reloads config without generation cleanup");
		return ok;
	}

	auto ExpectAuthenticationPreservesHostLocaleState() -> bool {
		EligibilityFlowFixture fixture;
		auto                   dependencies = MakeEligibilityFlowDependencies(&fixture);

		ConversationState state;
		struct pam_conv   conversation{
		    .conv        = TestConversation,
		    .appdata_ptr = &state,
		};
		ScopedPamHandle pam_handle;
		bool            ok = true;
		ok &=
		    expect(pam_handle.Start(&conversation) == PAM_SUCCESS, "locale test starts PAM handle");
		if (pam_handle.Get() == nullptr) {
			return false;
		}

		ScopedEnv         lc_all("LC_ALL");
		const char       *initial_locale_ptr = std::setlocale(LC_ALL, nullptr);
		const std::string initial_locale = initial_locale_ptr == nullptr ? "" : initial_locale_ptr;
		const char       *initial_domain_ptr = textdomain(nullptr);
		const std::string initial_domain = initial_domain_ptr == nullptr ? "" : initial_domain_ptr;
		const char       *host_environment_locale = "C.UTF-8";
		if (std::setlocale(LC_ALL, host_environment_locale) == nullptr) {
			host_environment_locale = "C.utf8";
		}
		const bool locale_available = std::setlocale(LC_ALL, host_environment_locale) != nullptr;
		if (locale_available) {
			setenv("LC_ALL", host_environment_locale, 1);
		}
		std::setlocale(LC_ALL, "C");
		textdomain("pam-host-test-domain");
		const std::string host_locale = std::setlocale(LC_ALL, nullptr);
		const std::string host_domain = textdomain(nullptr);

		const auto expect_host_state = [&](const char *scenario) -> void {
			if (locale_available) {
				ok &= expect(std::string(std::setlocale(LC_ALL, nullptr)) == host_locale,
				             std::string(scenario) + ": authentication restores host locale");
			}
			ok &= expect(std::string(textdomain(nullptr)) == host_domain,
			             std::string(scenario) + ": authentication restores host gettext domain");
		};

		ok &= expect(RunAuthenticationEntrypoint(pam_handle.Get(), {}, true,
		                                         {.context      = &dependencies,
		                                          .authenticate = IdentifyForTest}) == PAM_SUCCESS,
		             "successful authentication enters locale test flow");
		expect_host_state("successful authentication");

		fixture.runtime.disabled = true;
		ok &= expect(RunAuthenticationEntrypoint(
		                 pam_handle.Get(), {}, true,
		                 {.context = &dependencies, .authenticate = IdentifyForTest}) ==
		                 PAM_AUTHINFO_UNAVAIL,
		             "disabled early return preserves PAM behavior in locale test");
		expect_host_state("disabled early return");

		fixture.runtime.disabled      = false;
		fixture.eligibility.readiness = {
		    .status        = howdy::native::UserModelStatus::kInsecurePath,
		    .error_message = "locale test model failure",
		};
		ok &= expect(RunAuthenticationEntrypoint(
		                 pam_handle.Get(), {}, true,
		                 {.context = &dependencies, .authenticate = IdentifyForTest}) ==
		                 PAM_AUTHINFO_UNAVAIL,
		             "model failure returns through locale test flow");
		expect_host_state("model failure");

		if (!initial_domain.empty()) {
			textdomain(initial_domain.c_str());
		}
		if (!initial_locale.empty()) {
			std::setlocale(LC_ALL, initial_locale.c_str());
		}
		return ok;
	}

	auto ExpectAuthenticationEligibilityIntegration() -> bool {
		EligibilityFlowFixture fixture;
		auto                   dependencies = MakeEligibilityFlowDependencies(&fixture);

		ConversationState state;
		struct pam_conv   conversation{
		    .conv        = TestConversation,
		    .appdata_ptr = &state,
		};
		ScopedPamHandle pam_handle;
		bool            ok = true;
		ok &= expect(pam_handle.Start(&conversation) == PAM_SUCCESS,
		             "eligibility integration starts PAM handle");
		if (pam_handle.Get() == nullptr) {
			return false;
		}

		const auto reset_probe_calls = [&]() -> void {
			fixture.runtime.load_calls      = 0;
			fixture.eligibility.ssh_calls   = 0;
			fixture.eligibility.lid_calls   = 0;
			fixture.eligibility.model_calls = 0;
		};
		const auto expect_count = [&](const std::string &scenario, const char *probe, int actual,
		                              int expected) -> void {
			ok &= expect(actual == expected, scenario + ": expected " + probe + " calls " +
			                                     std::to_string(expected) + ", got " +
			                                     std::to_string(actual));
		};
		const auto expect_ineligible = [&](const char *scenario) -> void {
			const std::string name(scenario);
			const int         prompt_before = fixture.prompt.spawn_calls;
			reset_probe_calls();
			const int result = RunAuthenticationEntrypoint(
			    pam_handle.Get(), {}, true,
			    {.context = &dependencies, .authenticate = IdentifyForTest});
			ok &= expect(result == PAM_AUTHINFO_UNAVAIL,
			             name + ": ineligible result maps to PAM_AUTHINFO_UNAVAIL");
			ok &= expect(fixture.prompt.spawn_calls == prompt_before,
			             name + ": compare process is not spawned");
			ok &= expect(fixture.runtime.load_calls == 1,
			             name + ": runtime configuration loads before eligibility");
		};

		fixture.runtime.disabled      = true;
		fixture.eligibility.ssh       = true;
		fixture.eligibility.lid       = {.state = howdy::pam::runtime::LidState::kClosed};
		fixture.eligibility.readiness = {.status = howdy::native::UserModelStatus::kNoModel};
		expect_ineligible("globally disabled");
		expect_count("globally disabled", "SSH", fixture.eligibility.ssh_calls, 0);
		expect_count("globally disabled", "lid", fixture.eligibility.lid_calls, 0);
		expect_count("globally disabled", "model", fixture.eligibility.model_calls, 0);

		fixture.runtime.disabled      = false;
		fixture.eligibility.ssh       = true;
		fixture.eligibility.lid       = {.state = howdy::pam::runtime::LidState::kOpen};
		fixture.eligibility.readiness = {.status = howdy::native::UserModelStatus::kNoModel};
		expect_ineligible("SSH session");
		expect_count("SSH session", "SSH", fixture.eligibility.ssh_calls, 1);
		expect_count("SSH session", "lid", fixture.eligibility.lid_calls, 0);
		expect_count("SSH session", "model", fixture.eligibility.model_calls, 0);

		fixture.eligibility.ssh       = false;
		fixture.eligibility.lid       = {.state = howdy::pam::runtime::LidState::kClosed};
		fixture.eligibility.readiness = {.status = howdy::native::UserModelStatus::kNoModel};
		expect_ineligible("closed lid");
		expect_count("closed lid", "SSH", fixture.eligibility.ssh_calls, 1);
		expect_count("closed lid", "lid", fixture.eligibility.lid_calls, 1);
		expect_count("closed lid", "model", fixture.eligibility.model_calls, 0);

		fixture.eligibility.lid       = {.state = howdy::pam::runtime::LidState::kOpen};
		fixture.eligibility.readiness = {
		    .status = howdy::native::UserModelStatus::kInvalidUser,
		};
		expect_ineligible("invalid user");
		expect_count("invalid user", "SSH", fixture.eligibility.ssh_calls, 1);
		expect_count("invalid user", "lid", fixture.eligibility.lid_calls, 1);
		expect_count("invalid user", "model", fixture.eligibility.model_calls, 1);

		fixture.eligibility.readiness = {
		    .status = howdy::native::UserModelStatus::kNoModel,
		};
		expect_ineligible("missing model");
		expect_count("missing model", "SSH", fixture.eligibility.ssh_calls, 1);
		expect_count("missing model", "lid", fixture.eligibility.lid_calls, 1);
		expect_count("missing model", "model", fixture.eligibility.model_calls, 1);

		fixture.eligibility.readiness = {
		    .status        = howdy::native::UserModelStatus::kInsecurePath,
		    .error_message = "invalid model storage",
		};
		expect_ineligible("invalid model storage");
		expect_count("invalid model storage", "SSH", fixture.eligibility.ssh_calls, 1);
		expect_count("invalid model storage", "lid", fixture.eligibility.lid_calls, 1);
		expect_count("invalid model storage", "model", fixture.eligibility.model_calls, 1);

		fixture.eligibility.readiness = {
		    .status = howdy::native::UserModelStatus::kInsecurePath,
		};
		expect_ineligible("invalid model storage without diagnostic");
		expect_count("invalid model storage without diagnostic", "SSH",
		             fixture.eligibility.ssh_calls, 1);
		expect_count("invalid model storage without diagnostic", "lid",
		             fixture.eligibility.lid_calls, 1);
		expect_count("invalid model storage without diagnostic", "model",
		             fixture.eligibility.model_calls, 1);

		fixture.eligibility.lid = {
		    .status        = howdy::pam::runtime::LidProbeStatus::kError,
		    .state         = howdy::pam::runtime::LidState::kUnknown,
		    .error_message = "non-fatal lid diagnostic",
		};
		fixture.eligibility.readiness          = {.status = howdy::native::UserModelStatus::kOk};
		const int prompt_before_lid_diagnostic = fixture.prompt.spawn_calls;
		reset_probe_calls();
		ok &= expect(RunAuthenticationEntrypoint(pam_handle.Get(), {}, true,
		                                         {.context      = &dependencies,
		                                          .authenticate = IdentifyForTest}) == PAM_SUCCESS,
		             "eligible authentication continues after non-fatal lid diagnostic");
		ok &= expect(fixture.prompt.spawn_calls == prompt_before_lid_diagnostic + 1,
		             "non-fatal lid diagnostic does not suppress compare process");
		expect_count("eligible with lid diagnostic", "SSH", fixture.eligibility.ssh_calls, 1);
		expect_count("eligible with lid diagnostic", "lid", fixture.eligibility.lid_calls, 1);
		expect_count("eligible with lid diagnostic", "model", fixture.eligibility.model_calls, 1);

		fixture.eligibility.lid          = {.state = howdy::pam::runtime::LidState::kOpen};
		fixture.eligibility.readiness    = {.status = howdy::native::UserModelStatus::kOk};
		const int prompt_before_eligible = fixture.prompt.spawn_calls;
		reset_probe_calls();
		ok &= expect(RunAuthenticationEntrypoint(pam_handle.Get(), {}, true,
		                                         {.context      = &dependencies,
		                                          .authenticate = IdentifyForTest}) == PAM_SUCCESS,
		             "eligible authentication enters prompt coordination");
		ok &= expect(fixture.prompt.spawn_calls == prompt_before_eligible + 1,
		             "eligible authentication spawns compare process");
		expect_count("eligible", "SSH", fixture.eligibility.ssh_calls, 1);
		expect_count("eligible", "lid", fixture.eligibility.lid_calls, 1);
		expect_count("eligible", "model", fixture.eligibility.model_calls, 1);

		return ok;
	}

	auto ExpectPromptResultMapping() -> bool {
		using howdy::native::CompareExit;
		using howdy::pam::PamModuleArguments;
		using howdy::test::prompt_coordinator::FakeContext;

		std::array<const char *, 1> input_argv{"workaround=input"};
		const PamModuleArguments    input_arguments{
		    .flags = 0,
		    .argc  = 1,
		    .argv  = input_argv.data(),
		};

		struct PromptResultCase {
			const char *name;
			bool        blocked_child;
			int         child_exit;
			int         token_result;
			int         expected_result;
		};

		bool ok = true;

		const auto run_case = [&](const PromptResultCase &test_case) -> void {
			const auto [name, blocked_child, child_exit, token_result, expected_result] = test_case;
			EligibilityFlowFixture fixture;
			fixture.runtime.detection_notice = false;
			auto        dependencies         = MakeEligibilityFlowDependencies(&fixture);
			FakeContext prompt_context;
			dependencies.prompt_coordinator =
			    howdy::test::prompt_coordinator::Dependencies(&prompt_context);

			ConversationState state;
			struct pam_conv   conversation{
			    .conv        = TestConversation,
			    .appdata_ptr = &state,
			};
			ScopedPamHandle pam_handle;
			ok &= expect(pam_handle.Start(&conversation) == PAM_SUCCESS,
			             std::string(name) + ": starts PAM handle");
			if (pam_handle.Get() == nullptr) {
				return;
			}

			pid_t child_pid = -1;
			if (blocked_child) {
				child_pid = SpawnBlockedChild();
			} else {
				child_pid = SpawnExitingChild(child_exit);
			}
			ok &= expect(child_pid > 0, std::string(name) + ": spawns compare child");
			if (child_pid <= 0) {
				return;
			}
			prompt_context.next_child_pid       = child_pid;
			prompt_context.token_result         = token_result;
			prompt_context.token_waits_for_reap = !blocked_child;
			prompt_context.token_delay =
			    blocked_child ? std::chrono::milliseconds(50) : std::chrono::milliseconds(0);

			const int result = RunAuthenticationEntrypoint(
			    pam_handle.Get(), input_arguments, true,
			    {.context = &dependencies, .authenticate = IdentifyForTest});
			ok &= expect(result == expected_result,
			             std::string(name) + ": maps prompt result (got " + std::to_string(result) +
			                 ", auth calls " +
			                 std::to_string(prompt_context.auth_token_calls.load()) +
			                 ", terminate calls " +
			                 std::to_string(prompt_context.terminate_calls.load()) + ")");
			const bool child_handled = blocked_child ? prompt_context.terminate_calls == 1
			                                         : prompt_context.child_reaped_by_wait;
			ok &= expect(prompt_context.wait_calls == 1 && child_handled,
			             std::string(name) + ": compare child is handled");
			if (!child_handled) {
				(void)kill(child_pid, SIGKILL);
				(void)waitpid(child_pid, nullptr, 0);
			}
		};

		run_case({.name            = "password failure after blocked compare",
		          .blocked_child   = true,
		          .child_exit      = 0,
		          .token_result    = PAM_CONV_ERR,
		          .expected_result = PAM_CONV_ERR});
		run_case({.name            = "password success after blocked compare",
		          .blocked_child   = true,
		          .child_exit      = 0,
		          .token_result    = PAM_SUCCESS,
		          .expected_result = PAM_IGNORE});
		run_case({.name            = "compare failure before password",
		          .blocked_child   = false,
		          .child_exit      = static_cast<int>(CompareExit::kTooDark),
		          .token_result    = PAM_CONV_ERR,
		          .expected_result = PAM_AUTH_ERR});
		run_case({.name            = "compare failure with password success",
		          .blocked_child   = false,
		          .child_exit      = static_cast<int>(CompareExit::kTooDark),
		          .token_result    = PAM_SUCCESS,
		          .expected_result = PAM_IGNORE});

		{
			EligibilityFlowFixture fixture;
			fixture.runtime.detection_notice = false;
			auto        dependencies         = MakeEligibilityFlowDependencies(&fixture);
			FakeContext prompt_context;
			prompt_context.spawn_result = EIO;
			dependencies.prompt_coordinator =
			    howdy::test::prompt_coordinator::Dependencies(&prompt_context);
			ConversationState state;
			struct pam_conv   conversation{
			    .conv        = TestConversation,
			    .appdata_ptr = &state,
			};
			ScopedPamHandle pam_handle;
			ok &= expect(pam_handle.Start(&conversation) == PAM_SUCCESS,
			             "compare spawn failure starts PAM handle");
			if (pam_handle.Get() != nullptr) {
				ok &= expect(RunAuthenticationEntrypoint(
				                 pam_handle.Get(), input_arguments, true,
				                 {.context = &dependencies, .authenticate = IdentifyForTest}) ==
				                 PAM_SYSTEM_ERR,
				             "compare spawn failure maps to PAM_SYSTEM_ERR");
			}
		}

		{
			EligibilityFlowFixture fixture;
			fixture.runtime.detection_notice = false;
			fixture.runtime.timeout          = -3;
			auto        dependencies         = MakeEligibilityFlowDependencies(&fixture);
			FakeContext prompt_context;
			dependencies.prompt_coordinator =
			    howdy::test::prompt_coordinator::Dependencies(&prompt_context);
			ConversationState state;
			struct pam_conv   conversation{
			    .conv        = TestConversation,
			    .appdata_ptr = &state,
			};
			ScopedPamHandle pam_handle;
			ok &= expect(pam_handle.Start(&conversation) == PAM_SUCCESS,
			             "invalid coordinator timeout starts PAM handle");
			if (pam_handle.Get() != nullptr) {
				ok &= expect(RunAuthenticationEntrypoint(
				                 pam_handle.Get(), input_arguments, true,
				                 {.context = &dependencies, .authenticate = IdentifyForTest}) ==
				                 PAM_SYSTEM_ERR,
				             "invalid coordinator timeout maps to PAM_SYSTEM_ERR");
			}
		}
		return ok;
	}

	auto ExpectInvalidEligibilityDependenciesFailClosed() -> bool {
		EligibilityFlowFixture fixture;
		const auto             base = MakeEligibilityFlowDependencies(&fixture);
		bool                   ok   = true;

		const auto expect_invalid = [&](const char *scenario, auto invalidate) -> void {
			auto dependencies = base;
			invalidate(dependencies);
			const int runtime_calls = fixture.runtime.load_calls;
			const int prompt_calls  = fixture.prompt.spawn_calls;
			const int result =
			    howdy::pam::auth_flow::IdentifyWithDependencies(nullptr, {}, true, dependencies);
			const std::string name(scenario);
			ok &= expect(result == PAM_SYSTEM_ERR,
			             name + ": invalid dependency contract maps to PAM_SYSTEM_ERR");
			ok &= expect(fixture.runtime.load_calls == runtime_calls &&
			                 fixture.prompt.spawn_calls == prompt_calls &&
			                 fixture.eligibility.ssh_calls == 0 &&
			                 fixture.eligibility.lid_calls == 0 &&
			                 fixture.eligibility.model_calls == 0,
			             name + ": validation stops before runtime, probes and prompt");
		};

		expect_invalid("missing SSH-session callback", [](auto &dependencies) -> void {
			dependencies.eligibility.ssh_session_present = nullptr;
		});
		expect_invalid("missing lid-state callback", [](auto &dependencies) -> void {
			dependencies.eligibility.read_lid_state = nullptr;
		});
		expect_invalid("missing model-readiness callback", [](auto &dependencies) -> void {
			dependencies.eligibility.check_model_readiness = nullptr;
		});
		expect_invalid("missing runtime prepare callback", [](auto &dependencies) -> void {
			dependencies.runtime_session.prepare_runtime = nullptr;
		});
		expect_invalid("missing runtime effective-UID callback", [](auto &dependencies) -> void {
			dependencies.runtime_session.effective_uid = nullptr;
		});
		expect_invalid("missing compare wait callback", [](auto &dependencies) -> void {
			dependencies.prompt_coordinator.wait_for_compare_process = nullptr;
		});
		expect_invalid("missing compare cleanup callback", [](auto &dependencies) -> void {
			dependencies.prompt_coordinator.cancel_and_reap_compare_process = nullptr;
		});
		expect_invalid("missing input preflight callback", [](auto &dependencies) -> void {
			dependencies.prompt_coordinator.input_prompt_preflight = nullptr;
		});
		expect_invalid("missing prompt submitter callback", [](auto &dependencies) -> void {
			dependencies.prompt_coordinator.create_prompt_submitter = nullptr;
		});
		expect_invalid("missing native prompt callback", [](auto &dependencies) -> void {
			dependencies.prompt_coordinator.create_native_prompt = nullptr;
		});
		expect_invalid("missing secret conversation callback", [](auto &dependencies) -> void {
			dependencies.prompt_coordinator.create_secret_prompt_conversation = nullptr;
		});
		expect_invalid("missing auth-token callback", [](auto &dependencies) -> void {
			dependencies.prompt_coordinator.request_auth_token = nullptr;
		});
		expect_invalid("missing runtime config callback", [](auto &dependencies) -> void {
			dependencies.runtime_session.load_runtime_config = nullptr;
		});
		expect_invalid("missing prompt spawn callback", [](auto &dependencies) -> void {
			dependencies.prompt_coordinator.spawn_compare_process = nullptr;
		});

		return ok;
	}

}  // namespace

auto RunAuthFlowIntegrationTests() -> bool {
	bool ok = true;
	ok &= ExpectAuthenticationNoticeAndConversationGuards();
	ok &= ExpectRuntimeLoadFailurePaths();
	ok &= ExpectPromptResultMapping();
	ok &= ExpectAuthenticationPreservesHostLocaleState();
	ok &= ExpectAuthenticationEligibilityIntegration();
	ok &= ExpectInvalidEligibilityDependenciesFailClosed();
	return ok;
}
