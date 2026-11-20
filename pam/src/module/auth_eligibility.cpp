#include "module/auth_eligibility.hpp"

#include "config/runtime_config.hpp"
#include "protocol/runtime_descriptor_message.hpp"
#include "runtime/lid_probe.hpp"
#include "runtime/session_probe.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace howdy::pam::auth_eligibility {

	namespace {

		auto ProductionReadLidState(void *context) -> howdy::pam::runtime::LidStateResult {
			(void)context;
			return howdy::pam::runtime::ReadLidState();
		}

	}  // namespace

	auto ClassifyModelState(howdy::native::runtime_descriptor_message::ModelState state)
	    -> ModelCondition {
		using enum howdy::native::runtime_descriptor_message::ModelState;
		switch (state) {
			case kReady:
				return ModelCondition::kReady;
			case kMissing:
				return ModelCondition::kMissingModel;
			case kInvalidUser:
				return ModelCondition::kInvalidUser;
			case kInvalidStorage:
				return ModelCondition::kInvalidStorage;
		}
		return ModelCondition::kInvalidStorage;
	}

	auto ProductionAuthenticationEligibilityOperations() -> AuthenticationEligibilityOperations {
		auto operations = AuthenticationEligibilityOperations::Create(
		    nullptr, howdy::pam::runtime::ProductionSshSessionPresent, ProductionReadLidState);
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

	auto EvaluateAuthenticationEligibility(pam_handle_t                       *pamh,
	                                       const howdy::native::RuntimeConfig &config,
	                                       const char *username, const UserModelFinding &model,
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

		conditions.model_condition = model.condition;
		auto result                = DecideAuthenticationEligibility(config, conditions);
		result.diagnostic_message  = std::move(lid_diagnostic);
		if (result.status == AuthenticationEligibility::kInvalidModelStorage) {
			result.error_message = model.message;
		}
		return result;
	}

}  // namespace howdy::pam::auth_eligibility
