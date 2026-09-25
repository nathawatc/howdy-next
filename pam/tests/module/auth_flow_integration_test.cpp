#include "config/runtime_config_loader.hpp"
#include "module/auth_flow_test_groups.hpp"
#include "module/auth_flow_test_support.hpp"
#include "module/entrypoint.hpp"
#include "module/pam_options.hpp"
#include "module/production_entrypoint.hpp"
#include "paths.hpp"
#include "prompt/conversation_restore.hpp"
#include "prompt/native_prompt_conversation.hpp"
#include "prompt/observed_prompt_conversation.hpp"
#include "prompt/prompt_coordinator_fake.hpp"
#include "protocol/compare_exit.hpp"
#include "protocol/compare_invocation.hpp"
#include "runtime/compare_process.hpp"
#include "runtime/lid_probe.hpp"
#include "storage/user_model_status.hpp"
#include "support/process_test_support.hpp"
#include "test_support.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <clocale>
#include <cstdlib>
#include <optional>
#include <string>

#include <libintl.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include <security/pam_ext.h>

#include <sys/types.h>
#include <sys/wait.h>

namespace {
	using howdy::test::auth_flow::ConversationState;
	using howdy::test::auth_flow::EligibilityFlowFixture;
	using howdy::test::auth_flow::IdentifyForTest;
	using howdy::test::auth_flow::ScopedPamHandle;
	using howdy::test::auth_flow::TestConversation;
	using howdy::test::process::ChildReaped;
	using howdy::test::process::SpawnBlockedChild;
	using howdy::test::process::SpawnExitingChild;

	using howdy::pam::RunAuthenticationEntrypoint;
	using howdy::test::Expect;

	class ScopedEnv {
	public:
		ScopedEnv(ScopedEnv &&)                     = delete;
		auto operator=(ScopedEnv &&) -> ScopedEnv & = delete;

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
		struct pam_conv const  conversation{
		    .conv        = TestConversation,
		    .appdata_ptr = &state,
		};
		ScopedPamHandle pam_handle;
		bool            ok = true;
		ok &=
		    Expect(pam_handle.Start(&conversation) == PAM_SUCCESS, "notice test starts PAM handle");
		if (pam_handle.Get() == nullptr) {
			return false;
		}

		state.result = PAM_CONV_ERR;
		ok &= Expect(RunAuthenticationEntrypoint(pam_handle.Get(), {}, true,
		                                         {.context      = &dependencies,
		                                          .authenticate = IdentifyForTest}) == PAM_SUCCESS,
		             "detection notice conversation failure does not abort authentication");
		ok &= Expect(state.calls == 1 && state.last_msg_type == PAM_TEXT_INFO,
		             "enabled detection notice sends text message");

		fixture.runtime.detection_notice = false;
		state.result                     = PAM_SUCCESS;
		state.calls                      = 0;
		ok &= Expect(RunAuthenticationEntrypoint(pam_handle.Get(), {}, true,
		                                         {.context      = &dependencies,
		                                          .authenticate = IdentifyForTest}) == PAM_SUCCESS,
		             "disabled detection notice leaves authentication successful");
		ok &= Expect(state.calls == 0, "disabled detection notice sends no message");

		const struct pam_conv unavailable{
		    .conv        = nullptr,
		    .appdata_ptr = nullptr,
		};
		ok &= Expect(pam_set_item(pam_handle.Get(), PAM_CONV, &unavailable) == PAM_SUCCESS,
		             "installs unavailable PAM conversation");
		ok &=
		    Expect(RunAuthenticationEntrypoint(pam_handle.Get(), {}, true,
		                                       {.context      = &dependencies,
		                                        .authenticate = IdentifyForTest}) == PAM_SYSTEM_ERR,
		           "unavailable PAM conversation fails closed");
		return ok;
	}

	auto ExpectRuntimeLoadFailurePaths() -> bool {
		EligibilityFlowFixture fixture;
		auto                   dependencies = MakeEligibilityFlowDependencies(&fixture);
		ConversationState      state;
		struct pam_conv const  conversation{
		    .conv        = TestConversation,
		    .appdata_ptr = &state,
		};
		ScopedPamHandle pam_handle;
		bool            ok = true;
		ok &= Expect(pam_handle.Start(&conversation) == PAM_SUCCESS,
		             "runtime failure test starts PAM handle");
		if (pam_handle.Get() == nullptr) {
			return false;
		}

		fixture.runtime.initial_load_status = howdy::native::RuntimeConfigLoadStatus::kPathError;
		fixture.runtime.initial_error_code  = EACCES;
		fixture.runtime.effective_uid       = 1000;
		ok &=
		    Expect(RunAuthenticationEntrypoint(pam_handle.Get(), {}, true,
		                                       {.context      = &dependencies,
		                                        .authenticate = IdentifyForTest}) == PAM_SYSTEM_ERR,
		           "runtime staging preparation failure maps to PAM_SYSTEM_ERR");
		ok &= Expect(fixture.runtime.prepare_calls == 1 && fixture.runtime.load_calls == 1,
		             "runtime staging attempts preparation after inaccessible config");

		fixture.runtime.initial_load_status = howdy::native::RuntimeConfigLoadStatus::kParseError;
		fixture.runtime.initial_error_code  = 0;
		fixture.runtime.effective_uid       = 0;
		fixture.runtime.load_calls          = 0;
		ok &=
		    Expect(RunAuthenticationEntrypoint(pam_handle.Get(), {}, true,
		                                       {.context      = &dependencies,
		                                        .authenticate = IdentifyForTest}) == PAM_SYSTEM_ERR,
		           "runtime configuration failure maps to PAM_SYSTEM_ERR");
		ok &= Expect(fixture.runtime.prepare_calls == 1 && fixture.runtime.load_calls == 1,
		             "non-staging runtime failure skips preparation");

		fixture.runtime.initial_load_status = howdy::native::RuntimeConfigLoadStatus::kPathError;
		fixture.runtime.initial_error_code  = EACCES;
		fixture.runtime.effective_uid       = 1000;
		fixture.runtime.prepare_result      = true;
		fixture.runtime.staged_load_status  = howdy::native::RuntimeConfigLoadStatus::kOk;
		fixture.runtime.load_calls          = 0;
		ok &= Expect(RunAuthenticationEntrypoint(pam_handle.Get(), {}, true,
		                                         {.context      = &dependencies,
		                                          .authenticate = IdentifyForTest}) == PAM_SUCCESS,
		             "successful staged runtime continues authentication");
		ok &= Expect(fixture.runtime.prepare_calls == 2 && fixture.runtime.load_calls == 2,
		             "successful staged runtime reloads config without generation cleanup");
		return ok;
	}

	auto ExpectAuthenticationPreservesHostLocaleState() -> bool {
		EligibilityFlowFixture fixture;
		auto                   dependencies = MakeEligibilityFlowDependencies(&fixture);

		ConversationState     state;
		struct pam_conv const conversation{
		    .conv        = TestConversation,
		    .appdata_ptr = &state,
		};
		ScopedPamHandle pam_handle;
		bool            ok = true;
		ok &=
		    Expect(pam_handle.Start(&conversation) == PAM_SUCCESS, "locale test starts PAM handle");
		if (pam_handle.Get() == nullptr) {
			return false;
		}

		ScopedEnv const   lc_all("LC_ALL");
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
		ok &= Expect(std::setlocale(LC_ALL, "C") != nullptr, "locale integration enters C locale");
		textdomain("pam-host-test-domain");
		const std::string host_locale = std::setlocale(LC_ALL, nullptr);
		const std::string host_domain = textdomain(nullptr);

		const auto expect_host_state = [&](const char *scenario) -> void {
			if (locale_available) {
				ok &= Expect(std::string(std::setlocale(LC_ALL, nullptr)) == host_locale,
				             std::string(scenario) + ": authentication restores host locale");
			}
			ok &= Expect(std::string(textdomain(nullptr)) == host_domain,
			             std::string(scenario) + ": authentication restores host gettext domain");
		};

		ok &= Expect(RunAuthenticationEntrypoint(pam_handle.Get(), {}, true,
		                                         {.context      = &dependencies,
		                                          .authenticate = IdentifyForTest}) == PAM_SUCCESS,
		             "successful authentication enters locale test flow");
		expect_host_state("successful authentication");

		fixture.runtime.disabled = true;
		ok &= Expect(RunAuthenticationEntrypoint(
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
		ok &= Expect(RunAuthenticationEntrypoint(
		                 pam_handle.Get(), {}, true,
		                 {.context = &dependencies, .authenticate = IdentifyForTest}) ==
		                 PAM_AUTHINFO_UNAVAIL,
		             "model failure returns through locale test flow");
		expect_host_state("model failure");

		if (!initial_domain.empty()) {
			textdomain(initial_domain.c_str());
		}
		if (!initial_locale.empty()) {
			ok &= Expect(std::setlocale(LC_ALL, initial_locale.c_str()) != nullptr,
			             "locale integration restores initial locale");
		}
		return ok;
	}

	auto ExpectAuthenticationEligibilityIntegration() -> bool {
		EligibilityFlowFixture fixture;
		auto                   dependencies = MakeEligibilityFlowDependencies(&fixture);

		ConversationState     state;
		struct pam_conv const conversation{
		    .conv        = TestConversation,
		    .appdata_ptr = &state,
		};
		ScopedPamHandle pam_handle;
		bool            ok = true;
		ok &= Expect(pam_handle.Start(&conversation) == PAM_SUCCESS,
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
			ok &= Expect(actual == expected, scenario + ": expected " + probe + " calls " +
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
			ok &= Expect(result == PAM_AUTHINFO_UNAVAIL,
			             name + ": ineligible result maps to PAM_AUTHINFO_UNAVAIL");
			ok &= Expect(fixture.prompt.spawn_calls == prompt_before,
			             name + ": compare process is not spawned");
			ok &= Expect(fixture.runtime.load_calls == 1,
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
		ok &= Expect(RunAuthenticationEntrypoint(pam_handle.Get(), {}, true,
		                                         {.context      = &dependencies,
		                                          .authenticate = IdentifyForTest}) == PAM_SUCCESS,
		             "eligible authentication continues after non-fatal lid diagnostic");
		ok &= Expect(fixture.prompt.spawn_calls == prompt_before_lid_diagnostic + 1,
		             "non-fatal lid diagnostic does not suppress compare process");
		expect_count("eligible with lid diagnostic", "SSH", fixture.eligibility.ssh_calls, 1);
		expect_count("eligible with lid diagnostic", "lid", fixture.eligibility.lid_calls, 1);
		expect_count("eligible with lid diagnostic", "model", fixture.eligibility.model_calls, 1);

		fixture.eligibility.lid          = {.state = howdy::pam::runtime::LidState::kOpen};
		fixture.eligibility.readiness    = {.status = howdy::native::UserModelStatus::kOk};
		const int prompt_before_eligible = fixture.prompt.spawn_calls;
		reset_probe_calls();
		ok &= Expect(RunAuthenticationEntrypoint(pam_handle.Get(), {}, true,
		                                         {.context      = &dependencies,
		                                          .authenticate = IdentifyForTest}) == PAM_SUCCESS,
		             "eligible authentication enters prompt coordination");
		ok &= Expect(fixture.prompt.spawn_calls == prompt_before_eligible + 1,
		             "eligible authentication spawns compare process");
		expect_count("eligible", "SSH", fixture.eligibility.ssh_calls, 1);
		expect_count("eligible", "lid", fixture.eligibility.lid_calls, 1);
		expect_count("eligible", "model", fixture.eligibility.model_calls, 1);

		return ok;
	}

	struct ProductionPromptSpawnState {
		int   calls         = 0;
		int   child_exit    = EXIT_SUCCESS;
		pid_t child_pid     = -1;
		bool  request_valid = false;
	};

	auto SpawnProductionCompare(const howdy::pam::compare_process::SpawnRequest &request) -> int {
		auto *state = static_cast<ProductionPromptSpawnState *>(request.context);
		if (state == nullptr || request.child_pid == nullptr || request.argv.size() != 5 ||
		    request.envp.size() != 1) {
			return EINVAL;
		}
		++state->calls;
		auto        arguments     = request.argv;
		const char *program       = arguments.front();
		arguments                 = arguments.subspan(1);
		const char *config_option = arguments.front();
		arguments                 = arguments.subspan(1);
		const char *config_path   = arguments.front();
		arguments                 = arguments.subspan(1);
		const char *username      = arguments.front();
		arguments                 = arguments.subspan(1);
		const char *terminator    = arguments.front();
		state->request_valid =
		    request.path != nullptr && std::string_view(request.path) == kCompareProcessPath &&
		    request.actions != nullptr && program != nullptr &&
		    std::string_view(program) == kCompareProcessPath && config_option != nullptr &&
		    std::string_view(config_option) == howdy::native::kCompareConfigOption &&
		    config_path != nullptr && username != nullptr && terminator == nullptr &&
		    request.envp.front() == nullptr;
		const pid_t child_pid = fork();
		if (child_pid < 0) {
			return errno;
		}
		if (child_pid == 0) {
			_exit(state->child_exit);
		}
		state->child_pid   = child_pid;
		*request.child_pid = child_pid;
		return 0;
	}

	auto BeginObservedPrompt(void *context) -> howdy::pam::SecretPromptGeneration {
		(void)context;
		return 1;
	}

	void EndObservedPrompt(void *context, howdy::pam::SecretPromptGeneration generation) {
		(void)context;
		(void)generation;
	}

	auto ExpectProductionPromptComposition() -> bool {
		ProductionPromptSpawnState spawn_state;
		auto compare_operations    = howdy::pam::compare_process::ProductionOperations();
		compare_operations.context = &spawn_state;
		compare_operations.spawn   = SpawnProductionCompare;
		auto production_prompt =
		    howdy::pam::ProductionPromptCoordinatorOperations(compare_operations);

		EligibilityFlowFixture fixture;
		fixture.runtime.detection_notice = false;
		auto dependencies                = MakeEligibilityFlowDependencies(&fixture);
		dependencies.prompt_coordinator  = production_prompt;

		ConversationState     state;
		struct pam_conv const conversation{
		    .conv        = TestConversation,
		    .appdata_ptr = &state,
		};
		ScopedPamHandle pam_handle;
		bool            ok = true;
		ok &= Expect(pam_handle.Start(&conversation) == PAM_SUCCESS,
		             "production prompt composition starts PAM handle");
		if (pam_handle.Get() == nullptr) {
			return false;
		}

		ok &= Expect(RunAuthenticationEntrypoint(pam_handle.Get(), {}, false,
		                                         {.context      = &dependencies,
		                                          .authenticate = IdentifyForTest}) == PAM_SUCCESS,
		             "enabled authentication runs through production prompt composition");
		ok &= Expect(spawn_state.calls == 1 && spawn_state.request_valid,
		             "production compare adapter builds the expected launch request");
		ok &= Expect(spawn_state.child_pid > 0 && ChildReaped(spawn_state.child_pid),
		             "production compare wait adapter reaps the compare child");

		const pid_t wait_child = SpawnExitingChild(7);
		ok &= Expect(wait_child > 0, "production wait adapter test spawns child");
		if (wait_child > 0) {
			const int wait_status = production_prompt.WaitForCompareProcess(
			    wait_child, std::chrono::steady_clock::now() + std::chrono::seconds(1), nullptr,
			    nullptr);
			ok &= Expect(WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 7,
			             "production wait adapter preserves child exit status");
			ok &= Expect(ChildReaped(wait_child), "production wait adapter reaps direct child");
		}

		const pid_t blocked_child = SpawnBlockedChild();
		ok &= Expect(blocked_child > 0, "production cancel adapter test spawns child");
		if (blocked_child > 0) {
			production_prompt.CancelAndReapCompareProcess(blocked_child);
			ok &= Expect(ChildReaped(blocked_child),
			             "production cancel adapter terminates and reaps compare child");
		}

		const void *original_conv_item = nullptr;
		ok &= Expect(pam_get_item(pam_handle.Get(), PAM_CONV, &original_conv_item) == PAM_SUCCESS &&
		                 original_conv_item != nullptr,
		             "production conversation test reads original PAM conversation");
		const auto original_conv = original_conv_item == nullptr
		                               ? pam_conv{}
		                               : *static_cast<const pam_conv *>(original_conv_item);

		auto observed = production_prompt.CreateSecretPromptConversation(
		    pam_handle.Get(),
		    {.context = nullptr, .begin = BeginObservedPrompt, .end = EndObservedPrompt});
		ok &= Expect(dynamic_cast<howdy::pam::ObservedPromptConversation *>(observed.get()) !=
		                     nullptr &&
		                 observed->Available(),
		             "production secret-prompt adapter creates observed conversation");
		if (observed != nullptr && observed->Available()) {
			ok &= Expect(observed->Install() == PAM_SUCCESS,
			             "production observed conversation installs");
			ok &= Expect(observed->RestoreOriginal() ==
			                 howdy::pam::ConversationRestoreResult::kOriginalRestored,
			             "production observed conversation restores original callback");
			const void *restored_conv_item = nullptr;
			ok &= Expect(pam_get_item(pam_handle.Get(), PAM_CONV, &restored_conv_item) ==
			                     PAM_SUCCESS &&
			                 restored_conv_item != nullptr,
			             "restored PAM conversation remains available");
			if (restored_conv_item != nullptr) {
				const auto restored_conv = *static_cast<const pam_conv *>(restored_conv_item);
				ok &= Expect(restored_conv.conv == original_conv.conv &&
				                 restored_conv.appdata_ptr == original_conv.appdata_ptr,
				             "production observed conversation restores original identity");
			}
		}

		auto native_prompt = production_prompt.CreateNativePrompt(pam_handle.Get());
		ok &= Expect(dynamic_cast<NativePromptConversation *>(native_prompt.get()) != nullptr,
		             "production native-prompt adapter creates native conversation");

		ConversationState control_state;
		control_state.response_mode = howdy::test::auth_flow::ResponseMode::kSecret;
		struct pam_conv const control_conversation{
		    .conv        = TestConversation,
		    .appdata_ptr = &control_state,
		};
		ScopedPamHandle control_handle;
		ok &= Expect(control_handle.Start(&control_conversation) == PAM_SUCCESS,
		             "production auth-token control starts PAM handle");
		if (control_handle.Get() != nullptr) {
			const char *control_token = nullptr;
			const int   control_result =
			    pam_get_authtok(control_handle.Get(), PAM_AUTHTOK, &control_token, nullptr);
			state.response_mode = howdy::test::auth_flow::ResponseMode::kSecret;
			const auto [production_result, production_token] =
			    production_prompt.RequestAuthToken(pam_handle.Get());
			ok &= Expect(production_result == control_result &&
			                 (production_token != nullptr) == (control_token != nullptr),
			             "production auth-token adapter matches pam_get_authtok semantics");
			ok &= Expect(state.last_msg_type == control_state.last_msg_type &&
			                 state.last_msg_type == PAM_PROMPT_ECHO_OFF,
			             "production auth-token adapter uses PAM secret prompt conversation");
		}

		const auto entrypoint_dependencies = howdy::pam::ProductionEntrypointDependencies();
		ok &= Expect(entrypoint_dependencies.context == nullptr &&
		                 entrypoint_dependencies.authenticate == howdy::pam::ProductionAuthenticate,
		             "production entrypoint binds production authentication callback");
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
			    howdy::test::prompt_coordinator::Operations(&prompt_context);

			ConversationState     state;
			struct pam_conv const conversation{
			    .conv        = TestConversation,
			    .appdata_ptr = &state,
			};
			ScopedPamHandle pam_handle;
			ok &= Expect(pam_handle.Start(&conversation) == PAM_SUCCESS,
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
			ok &= Expect(child_pid > 0, std::string(name) + ": spawns compare child");
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
			ok &= Expect(result == expected_result,
			             std::string(name) + ": maps prompt result (got " + std::to_string(result) +
			                 ", auth calls " +
			                 std::to_string(prompt_context.auth_token_calls.load()) +
			                 ", terminate calls " +
			                 std::to_string(prompt_context.terminate_calls.load()) + ")");
			const bool child_handled = blocked_child ? prompt_context.terminate_calls == 1
			                                         : prompt_context.child_reaped_by_wait;
			ok &= Expect(prompt_context.wait_calls == 1 && child_handled,
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
			    howdy::test::prompt_coordinator::Operations(&prompt_context);
			ConversationState     state;
			struct pam_conv const conversation{
			    .conv        = TestConversation,
			    .appdata_ptr = &state,
			};
			ScopedPamHandle pam_handle;
			ok &= Expect(pam_handle.Start(&conversation) == PAM_SUCCESS,
			             "compare spawn failure starts PAM handle");
			if (pam_handle.Get() != nullptr) {
				ok &= Expect(RunAuthenticationEntrypoint(
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
			    howdy::test::prompt_coordinator::Operations(&prompt_context);
			ConversationState     state;
			struct pam_conv const conversation{
			    .conv        = TestConversation,
			    .appdata_ptr = &state,
			};
			ScopedPamHandle pam_handle;
			ok &= Expect(pam_handle.Start(&conversation) == PAM_SUCCESS,
			             "invalid coordinator timeout starts PAM handle");
			if (pam_handle.Get() != nullptr) {
				const int result = RunAuthenticationEntrypoint(
				    pam_handle.Get(), input_arguments, true,
				    {.context = &dependencies, .authenticate = IdentifyForTest});
				ok &= Expect(result == PAM_SYSTEM_ERR,
				             "invalid coordinator timeout maps to PAM_SYSTEM_ERR");
				ok &= Expect(fixture.runtime.load_calls == 1,
				             "invalid timeout loads runtime config once");
				ok &= Expect(fixture.eligibility.ssh_calls == 0 &&
				                 fixture.eligibility.lid_calls == 0 &&
				                 fixture.eligibility.model_calls == 0,
				             "invalid timeout stops before evaluating eligibility probes");
				ok &= Expect(state.calls == 0,
				             "invalid timeout stops before PAM conversation or detection notice");
				ok &= Expect(prompt_context.spawn_calls == 0,
				             "invalid timeout stops before spawning compare process");
			}
		}
		return ok;
	}

}  // namespace

auto RunAuthFlowIntegrationTests() -> bool {
	bool ok = true;
	ok &= ExpectAuthenticationNoticeAndConversationGuards();
	ok &= ExpectRuntimeLoadFailurePaths();
	ok &= ExpectPromptResultMapping();
	ok &= ExpectProductionPromptComposition();
	ok &= ExpectAuthenticationPreservesHostLocaleState();
	ok &= ExpectAuthenticationEligibilityIntegration();
	return ok;
}
