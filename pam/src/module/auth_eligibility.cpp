#include "module/auth_eligibility.hpp"

#include "config/runtime_config.hpp"
#include "runtime/session_probe.hpp"
#include "storage/user_model_status.hpp"

#include <stdexcept>
#include <string>
#include <utility>

#include <sys/types.h>

namespace howdy::pam::auth_eligibility {

	namespace {

		auto ProductionReadLidState(void *context) -> howdy::pam::runtime::LidStateResult {
			(void)context;
			return howdy::pam::runtime::ReadLidState();
		}

		auto ProductionCheckModelReadiness(void *context, const std::filesystem::path &models_dir,
		                                   const char *username)
		    -> howdy::native::UserModelReadinessResult {
			(void)context;
			if (username == nullptr || *username == '\0') {
				return {
				    .status        = howdy::native::UserModelStatus::kInvalidUser,
				    .error_message = "Invalid username",
				};
			}
			return howdy::native::CheckUserModelReadiness(models_dir, std::string(username),
			                                              static_cast<uid_t>(0));
		}

	}  // namespace

	auto ClassifyModelReadiness(const howdy::native::UserModelReadinessResult &readiness)
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

	auto ProductionAuthenticationEligibilityOperations() -> AuthenticationEligibilityOperations {
		auto operations = AuthenticationEligibilityOperations::Create(
		    nullptr, howdy::pam::runtime::ProductionSshSessionPresent, ProductionReadLidState,
		    ProductionCheckModelReadiness);
		if (!operations.has_value()) {
			throw std::logic_error(
			    "Failed to create production authentication eligibility operations");
		}
		return *operations;
	}

	auto DecideAuthenticationEligibility(const howdy::native::RuntimeConfig &config,
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

	auto EvaluateAuthenticationEligibility(pam_handle_t                              *pamh,
	                                       const howdy::native::RuntimeConfig        &config,
	                                       const char                                *username,
	                                       const std::filesystem::path               &models_dir,
	                                       const AuthenticationEligibilityOperations &operations)
	    -> AuthenticationEligibilityResult {
		AuthenticationConditions conditions;
		if (config.core.disabled) {
			return DecideAuthenticationEligibility(config, conditions);
		}

		if (config.core.abort_if_ssh) {
			conditions.ssh_session = operations.SshSessionPresent(pamh);
			if (conditions.ssh_session) {
				return DecideAuthenticationEligibility(config, conditions);
			}
		}

		std::string lid_diagnostic;
		if (config.core.abort_if_lid_closed) {
			const auto lid_result = operations.ReadLidState();
			if (lid_result.status == howdy::pam::runtime::LidProbeStatus::kError) {
				// Existing behavior logs lid-probe failures and continues authentication.
				lid_diagnostic = lid_result.error_message;
			}
			conditions.lid_state = lid_result.state;
			if (conditions.lid_state == howdy::pam::runtime::LidState::kClosed) {
				auto result               = DecideAuthenticationEligibility(config, conditions);
				result.diagnostic_message = std::move(lid_diagnostic);
				return result;
			}
		}

		if (username == nullptr || *username == '\0') {
			conditions.model_condition = ModelCondition::kInvalidUser;
			auto result                = DecideAuthenticationEligibility(config, conditions);
			result.diagnostic_message  = std::move(lid_diagnostic);
			return result;
		}

		const auto readiness       = operations.CheckModelReadiness(models_dir, username);
		conditions.model_condition = ClassifyModelReadiness(readiness);
		auto result                = DecideAuthenticationEligibility(config, conditions);
		result.diagnostic_message  = std::move(lid_diagnostic);
		if (result.status == AuthenticationEligibility::kInvalidModelStorage) {
			result.error_message = readiness.error_message;
		}
		return result;
	}

}  // namespace howdy::pam::auth_eligibility
