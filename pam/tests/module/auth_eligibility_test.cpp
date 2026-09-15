#include "module/auth_eligibility.hpp"
#include "test_support.hpp"

#include <array>
#include <filesystem>
#include <string>
#include <utility>

namespace {

	using howdy::native::RuntimeConfig;
	using howdy::native::UserModelReadinessResult;
	using howdy::native::UserModelStatus;
	using howdy::pam::auth_eligibility::AuthenticationConditions;
	using howdy::pam::auth_eligibility::AuthenticationEligibility;
	using howdy::pam::auth_eligibility::AuthenticationEligibilityDependencies;
	using howdy::pam::auth_eligibility::ClassifyModelReadiness;
	using howdy::pam::auth_eligibility::DecideAuthenticationEligibility;
	using howdy::pam::auth_eligibility::EvaluateAuthenticationEligibility;
	using howdy::pam::auth_eligibility::ModelCondition;
	using howdy::pam::runtime::LidProbeStatus;
	using howdy::pam::runtime::LidState;
	using howdy::pam::runtime::LidStateResult;
	using howdy::test::Expect;

	struct PolicyCase {
		const char               *name;
		bool                      disabled;
		bool                      abort_if_ssh;
		bool                      abort_if_lid_closed;
		bool                      ssh_session;
		LidState                  lid_state;
		ModelCondition            model_condition;
		AuthenticationEligibility expected;
	};

	auto ExpectPurePolicy() -> bool {
		const std::array cases = {
		    PolicyCase{.name                = "disabled",
		               .disabled            = true,
		               .abort_if_ssh        = true,
		               .abort_if_lid_closed = true,
		               .ssh_session         = true,
		               .lid_state           = LidState::kClosed,
		               .model_condition     = ModelCondition::kInvalidUser,
		               .expected            = AuthenticationEligibility::kDisabled},
		    PolicyCase{.name                = "SSH session",
		               .disabled            = false,
		               .abort_if_ssh        = true,
		               .abort_if_lid_closed = true,
		               .ssh_session         = true,
		               .lid_state           = LidState::kOpen,
		               .model_condition     = ModelCondition::kReady,
		               .expected            = AuthenticationEligibility::kSshSession},
		    PolicyCase{.name                = "SSH allowed by config",
		               .disabled            = false,
		               .abort_if_ssh        = false,
		               .abort_if_lid_closed = true,
		               .ssh_session         = true,
		               .lid_state           = LidState::kOpen,
		               .model_condition     = ModelCondition::kReady,
		               .expected            = AuthenticationEligibility::kEligible},
		    PolicyCase{.name                = "closed lid",
		               .disabled            = false,
		               .abort_if_ssh        = false,
		               .abort_if_lid_closed = true,
		               .ssh_session         = false,
		               .lid_state           = LidState::kClosed,
		               .model_condition     = ModelCondition::kReady,
		               .expected            = AuthenticationEligibility::kClosedLid},
		    PolicyCase{.name                = "closed lid allowed by config",
		               .disabled            = false,
		               .abort_if_ssh        = false,
		               .abort_if_lid_closed = false,
		               .ssh_session         = false,
		               .lid_state           = LidState::kClosed,
		               .model_condition     = ModelCondition::kReady,
		               .expected            = AuthenticationEligibility::kEligible},
		    PolicyCase{.name                = "invalid user",
		               .disabled            = false,
		               .abort_if_ssh        = false,
		               .abort_if_lid_closed = false,
		               .ssh_session         = false,
		               .lid_state           = LidState::kUnknown,
		               .model_condition     = ModelCondition::kInvalidUser,
		               .expected            = AuthenticationEligibility::kInvalidUser},
		    PolicyCase{.name                = "missing model",
		               .disabled            = false,
		               .abort_if_ssh        = false,
		               .abort_if_lid_closed = false,
		               .ssh_session         = false,
		               .lid_state           = LidState::kUnknown,
		               .model_condition     = ModelCondition::kMissingModel,
		               .expected            = AuthenticationEligibility::kMissingModel},
		    PolicyCase{.name                = "invalid model storage",
		               .disabled            = false,
		               .abort_if_ssh        = false,
		               .abort_if_lid_closed = false,
		               .ssh_session         = false,
		               .lid_state           = LidState::kUnknown,
		               .model_condition     = ModelCondition::kInvalidStorage,
		               .expected            = AuthenticationEligibility::kInvalidModelStorage},
		    PolicyCase{.name                = "unchecked model condition fails safely",
		               .disabled            = false,
		               .abort_if_ssh        = false,
		               .abort_if_lid_closed = false,
		               .ssh_session         = false,
		               .lid_state           = LidState::kUnknown,
		               .model_condition     = ModelCondition::kUnchecked,
		               .expected            = AuthenticationEligibility::kRuntimeError},
		    PolicyCase{.name                = "eligible",
		               .disabled            = false,
		               .abort_if_ssh        = false,
		               .abort_if_lid_closed = false,
		               .ssh_session         = false,
		               .lid_state           = LidState::kOpen,
		               .model_condition     = ModelCondition::kReady,
		               .expected            = AuthenticationEligibility::kEligible},
		    PolicyCase{.name                = "disabled takes precedence",
		               .disabled            = true,
		               .abort_if_ssh        = true,
		               .abort_if_lid_closed = true,
		               .ssh_session         = true,
		               .lid_state           = LidState::kClosed,
		               .model_condition     = ModelCondition::kUnchecked,
		               .expected            = AuthenticationEligibility::kDisabled},
		    PolicyCase{.name                = "SSH takes precedence over lid and model",
		               .disabled            = false,
		               .abort_if_ssh        = true,
		               .abort_if_lid_closed = true,
		               .ssh_session         = true,
		               .lid_state           = LidState::kClosed,
		               .model_condition     = ModelCondition::kMissingModel,
		               .expected            = AuthenticationEligibility::kSshSession},
		    PolicyCase{.name                = "closed lid takes precedence over model",
		               .disabled            = false,
		               .abort_if_ssh        = false,
		               .abort_if_lid_closed = true,
		               .ssh_session         = false,
		               .lid_state           = LidState::kClosed,
		               .model_condition     = ModelCondition::kMissingModel,
		               .expected            = AuthenticationEligibility::kClosedLid},
		};

		bool ok = true;
		for (const auto &test_case : cases) {
			RuntimeConfig config;
			config.core.disabled            = test_case.disabled;
			config.core.abort_if_ssh        = test_case.abort_if_ssh;
			config.core.abort_if_lid_closed = test_case.abort_if_lid_closed;
			const AuthenticationConditions conditions{
			    .ssh_session     = test_case.ssh_session,
			    .lid_state       = test_case.lid_state,
			    .model_condition = test_case.model_condition,
			};
			const auto result = DecideAuthenticationEligibility(config, conditions);
			ok &= Expect(result.status == test_case.expected, test_case.name);
		}
		return ok;
	}

	auto ExpectModelClassification() -> bool {
		const std::array cases = {
		    std::pair{UserModelStatus::kOk, ModelCondition::kReady},
		    std::pair{UserModelStatus::kInvalidUser, ModelCondition::kInvalidUser},
		    std::pair{UserModelStatus::kNoModel, ModelCondition::kMissingModel},
		    std::pair{UserModelStatus::kNoModelDirectory, ModelCondition::kMissingModel},
		    std::pair{UserModelStatus::kIncompatibleBackend, ModelCondition::kInvalidStorage},
		    std::pair{UserModelStatus::kIncompatibleMetric, ModelCondition::kInvalidStorage},
		    std::pair{UserModelStatus::kIncompatibleModel, ModelCondition::kInvalidStorage},
		    std::pair{UserModelStatus::kParseError, ModelCondition::kInvalidStorage},
		    std::pair{UserModelStatus::kInvalidShape, ModelCondition::kInvalidStorage},
		    std::pair{UserModelStatus::kOversized, ModelCondition::kInvalidStorage},
		    std::pair{UserModelStatus::kInsecurePath, ModelCondition::kInvalidStorage},
		    std::pair{UserModelStatus::kLockFailed, ModelCondition::kInvalidStorage},
		    std::pair{UserModelStatus::kWriteFailed, ModelCondition::kInvalidStorage},
		    std::pair{UserModelStatus::kDeleteFailed, ModelCondition::kInvalidStorage},
		    std::pair{UserModelStatus::kDurabilityUncertain, ModelCondition::kInvalidStorage},
		    std::pair{UserModelStatus::kCommitStateUncertain, ModelCondition::kInvalidStorage},
		    std::pair{UserModelStatus::kDirectoryCreateFailed, ModelCondition::kInvalidStorage},
		    std::pair{UserModelStatus::kModelNotFound, ModelCondition::kInvalidStorage},
		    std::pair{UserModelStatus::kModelChanged, ModelCondition::kInvalidStorage},
		};

		bool ok = true;
		for (const auto &[status, expected] : cases) {
			ok &= Expect(ClassifyModelReadiness({.status = status}) == expected,
			             "storage readiness maps to eligibility condition");
		}
		return ok;
	}

	struct CollectorState {
		bool                     ssh = false;
		LidStateResult           lid{.status = LidProbeStatus::kOk, .state = LidState::kOpen};
		UserModelReadinessResult readiness{.status = UserModelStatus::kOk};
		int                      ssh_calls   = 0;
		int                      lid_calls   = 0;
		int                      model_calls = 0;
		std::filesystem::path    observed_models_dir;
		std::string              observed_username;
	};

	auto FakeSsh(void *context, pam_handle_t *pamh) -> bool {
		(void)pamh;
		auto *state = static_cast<CollectorState *>(context);
		++state->ssh_calls;
		return state->ssh;
	}

	auto FakeLid(void *context) -> LidStateResult {
		auto *state = static_cast<CollectorState *>(context);
		++state->lid_calls;
		return state->lid;
	}

	auto FakeModel(void *context, const std::filesystem::path &models_dir, const char *username)
	    -> UserModelReadinessResult {
		auto *state = static_cast<CollectorState *>(context);
		++state->model_calls;
		state->observed_models_dir = models_dir;
		state->observed_username   = username == nullptr ? "" : username;
		return state->readiness;
	}

	auto Dependencies(CollectorState *state) -> AuthenticationEligibilityDependencies {
		return {
		    .context               = state,
		    .ssh_session_present   = FakeSsh,
		    .read_lid_state        = FakeLid,
		    .check_model_readiness = FakeModel,
		};
	}

	auto ExpectCollector() -> bool {
		bool ok = true;

		RuntimeConfig config;
		config.core.abort_if_ssh        = true;
		config.core.abort_if_lid_closed = true;
		config.core.disabled            = false;

		CollectorState state;
		const auto     deps = Dependencies(&state);
		const auto     invalid_dependencies =
		    EvaluateAuthenticationEligibility(nullptr, config, "alice", "/models", {});
		ok &= Expect(invalid_dependencies.status == AuthenticationEligibility::kRuntimeError,
		             "invalid eligibility dependencies produce typed runtime failure");

		const auto reset_probe_calls = [&]() -> void {
			state.ssh_calls   = 0;
			state.lid_calls   = 0;
			state.model_calls = 0;
		};

		config.core.disabled = true;
		reset_probe_calls();
		const auto disabled =
		    EvaluateAuthenticationEligibility(nullptr, config, "alice", "/models", deps);
		ok &= Expect(disabled.status == AuthenticationEligibility::kDisabled,
		             "disabled evaluation returns disabled");
		ok &= Expect(state.ssh_calls == 0 && state.lid_calls == 0 && state.model_calls == 0,
		             "disabled evaluation never calls SSH, lid or model probes");

		config.core.disabled            = false;
		config.core.abort_if_ssh        = true;
		config.core.abort_if_lid_closed = true;
		state.ssh                       = true;
		reset_probe_calls();
		const auto ssh =
		    EvaluateAuthenticationEligibility(nullptr, config, "alice", "/models", deps);
		ok &= Expect(ssh.status == AuthenticationEligibility::kSshSession,
		             "SSH rejection returns SSH-session eligibility");
		ok &= Expect(state.ssh_calls == 1 && state.lid_calls == 0 && state.model_calls == 0,
		             "SSH rejection skips lid and model probes");

		state.ssh = false;
		state.lid = {.status = LidProbeStatus::kOk, .state = LidState::kClosed};
		reset_probe_calls();
		const auto closed_lid =
		    EvaluateAuthenticationEligibility(nullptr, config, "alice", "/models", deps);
		ok &= Expect(closed_lid.status == AuthenticationEligibility::kClosedLid,
		             "closed-lid rejection returns closed-lid eligibility");
		ok &= Expect(state.ssh_calls == 1 && state.lid_calls == 1 && state.model_calls == 0,
		             "closed-lid rejection skips model readiness");

		state.lid = {
		    .status        = LidProbeStatus::kError,
		    .state         = LidState::kClosed,
		    .error_message = "one lid source was unreadable",
		};
		reset_probe_calls();
		const auto closed_lid_with_diagnostic =
		    EvaluateAuthenticationEligibility(nullptr, config, "alice", "/models", deps);
		ok &= Expect(closed_lid_with_diagnostic.status == AuthenticationEligibility::kClosedLid,
		             "closed state still rejects when lid probe has read diagnostic");
		ok &= Expect(closed_lid_with_diagnostic.diagnostic_message ==
		                     "one lid source was unreadable" &&
		                 state.ssh_calls == 1 && state.lid_calls == 1 && state.model_calls == 0,
		             "closed-lid diagnostic remains separate and skips model readiness");

		state.lid       = {.status = LidProbeStatus::kOk, .state = LidState::kOpen};
		state.readiness = {.status = UserModelStatus::kOk};
		reset_probe_calls();
		const auto eligible =
		    EvaluateAuthenticationEligibility(nullptr, config, "alice", "/staged/models", deps);
		ok &= Expect(eligible.status == AuthenticationEligibility::kEligible,
		             "open lid evaluation reaches model readiness and becomes eligible");
		ok &= Expect(state.ssh_calls == 1 && state.lid_calls == 1 && state.model_calls == 1 &&
		                 state.observed_models_dir == "/staged/models" &&
		                 state.observed_username == "alice",
		             "eligible evaluation calls probes once with exact model path and username");

		state.lid = {.status = LidProbeStatus::kOk, .state = LidState::kUnknown};
		reset_probe_calls();
		const auto unknown_lid =
		    EvaluateAuthenticationEligibility(nullptr, config, "alice", "/models", deps);
		ok &= Expect(unknown_lid.status == AuthenticationEligibility::kEligible,
		             "unknown lid state still reaches model readiness");
		ok &= Expect(state.ssh_calls == 1 && state.lid_calls == 1 && state.model_calls == 1,
		             "unknown lid state calls model readiness exactly once");

		config.core.abort_if_ssh = false;
		state.ssh                = true;
		state.lid                = {.status = LidProbeStatus::kOk, .state = LidState::kOpen};
		state.readiness          = {.status = UserModelStatus::kOk};
		reset_probe_calls();
		const auto ssh_allowed =
		    EvaluateAuthenticationEligibility(nullptr, config, "alice", "/models", deps);
		ok &= Expect(ssh_allowed.status == AuthenticationEligibility::kEligible,
		             "SSH session is allowed when abort_if_ssh is disabled");
		ok &= Expect(state.ssh_calls == 0 && state.lid_calls == 1 && state.model_calls == 1,
		             "disabled SSH rule skips SSH probe but still evaluates lid and model");

		config.core.abort_if_ssh        = true;
		config.core.abort_if_lid_closed = false;
		state.ssh                       = false;
		state.lid                       = {
		    .status        = LidProbeStatus::kError,
		    .state         = LidState::kUnknown,
		    .error_message = "lid probe must not run",
		};
		state.readiness = {.status = UserModelStatus::kOk};
		reset_probe_calls();
		const auto lid_allowed =
		    EvaluateAuthenticationEligibility(nullptr, config, "alice", "/models", deps);
		ok &= Expect(lid_allowed.status == AuthenticationEligibility::kEligible,
		             "lid probe is skipped when abort_if_lid_closed is disabled");
		ok &= Expect(state.ssh_calls == 1 && state.lid_calls == 0 && state.model_calls == 1 &&
		                 lid_allowed.diagnostic_message.empty(),
		             "disabled lid rule skips lid probe and produces no lid diagnostic");

		config.core.abort_if_ssh        = false;
		config.core.abort_if_lid_closed = false;
		state.ssh                       = true;
		state.lid                       = {
		    .status        = LidProbeStatus::kError,
		    .state         = LidState::kUnknown,
		    .error_message = "lid probe must not run",
		};
		state.readiness = {.status = UserModelStatus::kOk};
		reset_probe_calls();
		const auto probes_disabled =
		    EvaluateAuthenticationEligibility(nullptr, config, "alice", "/models", deps);
		ok &= Expect(probes_disabled.status == AuthenticationEligibility::kEligible,
		             "both optional probes disabled still allow ready model");
		ok &= Expect(state.ssh_calls == 0 && state.lid_calls == 0 && state.model_calls == 1,
		             "both disabled probes are skipped and model readiness runs directly");

		config.core.abort_if_ssh        = true;
		config.core.abort_if_lid_closed = true;
		state.ssh                       = false;
		state.lid                       = {.status = LidProbeStatus::kOk, .state = LidState::kOpen};
		state.readiness                 = {
		    .status        = UserModelStatus::kInsecurePath,
		    .error_message = "insecure model storage",
		};
		reset_probe_calls();
		const auto invalid_storage =
		    EvaluateAuthenticationEligibility(nullptr, config, "alice", "/models", deps);
		ok &= Expect(invalid_storage.status == AuthenticationEligibility::kInvalidModelStorage,
		             "invalid model storage is terminal eligibility result");
		ok &= Expect(invalid_storage.error_message == "insecure model storage" &&
		                 invalid_storage.diagnostic_message.empty() && state.ssh_calls == 1 &&
		                 state.lid_calls == 1 && state.model_calls == 1,
		             "model readiness error remains terminal without unrelated diagnostic");

		state.lid = {
		    .status        = LidProbeStatus::kError,
		    .state         = LidState::kUnknown,
		    .error_message = "lid read failed",
		};
		state.readiness = {.status = UserModelStatus::kOk};
		reset_probe_calls();
		const auto lid_error =
		    EvaluateAuthenticationEligibility(nullptr, config, "alice", "/models", deps);
		ok &= Expect(lid_error.status == AuthenticationEligibility::kEligible,
		             "lid probe failure continues to model readiness");
		ok &= Expect(lid_error.error_message.empty() &&
		                 lid_error.diagnostic_message == "lid read failed" &&
		                 state.ssh_calls == 1 && state.lid_calls == 1 && state.model_calls == 1,
		             "lid failure keeps independent diagnostic and calls model once");

		state.readiness = {
		    .status        = UserModelStatus::kInsecurePath,
		    .error_message = "terminal model failure",
		};
		reset_probe_calls();
		const auto simultaneous_errors =
		    EvaluateAuthenticationEligibility(nullptr, config, "alice", "/models", deps);
		ok &= Expect(simultaneous_errors.status == AuthenticationEligibility::kInvalidModelStorage,
		             "terminal model failure wins eligibility decision");
		ok &= Expect(simultaneous_errors.error_message == "terminal model failure" &&
		                 simultaneous_errors.diagnostic_message == "lid read failed",
		             "terminal model error and lid diagnostic remain separate");

		state.lid = {.status = LidProbeStatus::kOk, .state = LidState::kOpen};
		reset_probe_calls();
		const auto invalid_user =
		    EvaluateAuthenticationEligibility(nullptr, config, nullptr, "/models", deps);
		ok &= Expect(invalid_user.status == AuthenticationEligibility::kInvalidUser,
		             "null username becomes invalid-user eligibility");
		ok &= Expect(state.model_calls == 0,
		             "invalid username skips model readiness callback entirely");

		return ok;
	}

}  // namespace

auto main() -> int {
	bool ok = true;
	ok &= ExpectPurePolicy();
	ok &= ExpectModelClassification();
	ok &= ExpectCollector();
	return ok ? 0 : 1;
}
