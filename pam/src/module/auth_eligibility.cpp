#include "module/auth_eligibility.hpp"

#include "config/runtime_config.hpp"
#include "runtime/session_probe.hpp"
#include "storage/user_model_status.hpp"

#include <string>
#include <utility>

#include <sys/types.h>

namespace howdy::pam::auth_eligibility {

	namespace {

		auto invalid_dependencies() -> AuthenticationEligibilityResult {
			return {
			    .status        = AuthenticationEligibility::kRuntimeError,
			    .error_message = "Authentication eligibility dependencies are invalid",
			};
		}

		auto production_read_lid_state(void *context) -> howdy::pam::runtime::LidStateResult {
			(void)context;
			return howdy::pam::runtime::read_lid_state();
		}

		auto production_check_model_readiness(void                        *context,
		                                      const std::filesystem::path &models_dir,
		                                      const char                  *username)
		    -> howdy::native::UserModelReadinessResult {
			(void)context;
			if (username == nullptr || *username == '\0') {
				return {
				    .status        = howdy::native::UserModelStatus::kInvalidUser,
				    .error_message = "Invalid username",
				};
			}
			return howdy::native::check_user_model_readiness(models_dir, std::string(username),
			                                                 static_cast<uid_t>(0));
		}

	}  // namespace

	auto classify_model_readiness(const howdy::native::UserModelReadinessResult &readiness)
	    -> ModelCondition {
		using enum howdy::native::UserModelStatus;
		switch (readiness.status) {
			case kOk:
				return ModelCondition::kReady;
			case kInvalidUser:
				return ModelCondition::kInvalidUser;
			case kNoModel:
			case kNoModelDirectory:
				return ModelCondition::kMissingModel;
			case kIncompatibleBackend:
			case kIncompatibleMetric:
			case kIncompatibleModel:
			case kParseError:
			case kInvalidShape:
			case kOversized:
			case kInsecurePath:
			case kLockFailed:
			case kWriteFailed:
			case kAtomicExchangeUnsupported:
			case kDeleteFailed:
			case kDurabilityUncertain:
			case kCommitStateUncertain:
			case kDirectoryCreateFailed:
			case kModelNotFound:
			case kModelChanged:
				return ModelCondition::kInvalidStorage;
		}
		return ModelCondition::kInvalidStorage;
	}

	auto production_authentication_eligibility_dependencies()
	    -> AuthenticationEligibilityDependencies {
		return {
		    .context               = nullptr,
		    .ssh_session_present   = howdy::pam::runtime::production_ssh_session_present,
		    .read_lid_state        = production_read_lid_state,
		    .check_model_readiness = production_check_model_readiness,
		};
	}

	auto decide_authentication_eligibility(const howdy::native::RuntimeConfig &config,
	                                       const AuthenticationConditions     &conditions)
	    -> AuthenticationEligibilityResult {
		if (config.core.disabled) {
			return {.status = AuthenticationEligibility::kDisabled};
		}

		if (config.core.abort_if_ssh && conditions.ssh_session) {
			return {.status = AuthenticationEligibility::kSshSession};
		}

		if (config.core.abort_if_lid_closed &&
		    conditions.lid_state == howdy::pam::runtime::LidState::kClosed) {
			return {.status = AuthenticationEligibility::kClosedLid};
		}

		switch (conditions.model_condition) {
			case ModelCondition::kReady:
				return {.status = AuthenticationEligibility::kEligible};
			case ModelCondition::kInvalidUser:
				return {.status = AuthenticationEligibility::kInvalidUser};
			case ModelCondition::kMissingModel:
				return {.status = AuthenticationEligibility::kMissingModel};
			case ModelCondition::kInvalidStorage:
				return {.status = AuthenticationEligibility::kInvalidModelStorage};
			case ModelCondition::kUnchecked:
				return {.status = AuthenticationEligibility::kRuntimeError};
		}
		return {.status = AuthenticationEligibility::kRuntimeError};
	}

	auto evaluate_authentication_eligibility(
	    pam_handle_t *pamh, const howdy::native::RuntimeConfig &config, const char *username,
	    const std::filesystem::path                 &models_dir,
	    const AuthenticationEligibilityDependencies &dependencies)
	    -> AuthenticationEligibilityResult {
		if (dependencies.ssh_session_present == nullptr || dependencies.read_lid_state == nullptr ||
		    dependencies.check_model_readiness == nullptr) {
			return invalid_dependencies();
		}

		AuthenticationConditions conditions;
		if (config.core.disabled) {
			return decide_authentication_eligibility(config, conditions);
		}

		if (config.core.abort_if_ssh) {
			conditions.ssh_session = dependencies.ssh_session_present(dependencies.context, pamh);
			if (conditions.ssh_session) {
				return decide_authentication_eligibility(config, conditions);
			}
		}

		std::string lid_diagnostic;
		if (config.core.abort_if_lid_closed) {
			const auto lid_result = dependencies.read_lid_state(dependencies.context);
			if (lid_result.status == howdy::pam::runtime::LidProbeStatus::kError) {
				// Existing behavior logs lid-probe failures and continues authentication.
				lid_diagnostic = lid_result.error_message;
			}
			conditions.lid_state = lid_result.state;
			if (conditions.lid_state == howdy::pam::runtime::LidState::kClosed) {
				auto result               = decide_authentication_eligibility(config, conditions);
				result.diagnostic_message = std::move(lid_diagnostic);
				return result;
			}
		}

		if (username == nullptr || *username == '\0') {
			conditions.model_condition = ModelCondition::kInvalidUser;
			auto result                = decide_authentication_eligibility(config, conditions);
			result.diagnostic_message  = std::move(lid_diagnostic);
			return result;
		}

		const auto readiness =
		    dependencies.check_model_readiness(dependencies.context, models_dir, username);
		conditions.model_condition = classify_model_readiness(readiness);
		auto result                = decide_authentication_eligibility(config, conditions);
		result.diagnostic_message  = std::move(lid_diagnostic);
		if (result.status == AuthenticationEligibility::kInvalidModelStorage) {
			result.error_message = readiness.error_message;
		}
		return result;
	}

}  // namespace howdy::pam::auth_eligibility
