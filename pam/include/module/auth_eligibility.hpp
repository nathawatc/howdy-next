#pragma once

#include "config/runtime_config.hpp"
#include "runtime/lid_probe.hpp"
#include "storage/user_model_readiness.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

#include <security/pam_appl.h>

namespace howdy::pam::auth_eligibility {

	enum class ModelCondition : std::uint8_t {
		kUnchecked,
		kReady,
		kInvalidUser,
		kMissingModel,
		kInvalidStorage,
	};

	struct AuthenticationConditions {
		bool                          ssh_session     = false;
		howdy::pam::runtime::LidState lid_state       = howdy::pam::runtime::LidState::kUnknown;
		ModelCondition                model_condition = ModelCondition::kUnchecked;
	};

	enum class AuthenticationEligibility : std::uint8_t {
		kEligible,
		kDisabled,
		kSshSession,
		kClosedLid,
		kInvalidUser,
		kMissingModel,
		kInvalidModelStorage,
		kRuntimeError,
	};

	struct AuthenticationEligibilityResult {
		AuthenticationEligibility status = AuthenticationEligibility::kRuntimeError;
		std::string               error_message;
		std::string               diagnostic_message;
	};

	struct AuthenticationEligibilityDependencies {
		void *context = nullptr;

		bool (*ssh_session_present)(void *context, pam_handle_t *pamh)       = nullptr;
		howdy::pam::runtime::LidStateResult (*read_lid_state)(void *context) = nullptr;
		howdy::native::UserModelReadinessResult (*check_model_readiness)(
		    void *context, const std::filesystem::path &models_dir, const char *username) = nullptr;
	};

	auto ClassifyModelReadiness(const howdy::native::UserModelReadinessResult &readiness)
	    -> ModelCondition;

	auto ProductionAuthenticationEligibilityDependencies() -> AuthenticationEligibilityDependencies;

	auto DecideAuthenticationEligibility(const howdy::native::RuntimeConfig &config,
	                                     const AuthenticationConditions     &conditions)
	    -> AuthenticationEligibilityResult;

	auto
	EvaluateAuthenticationEligibility(pam_handle_t                       *pamh,
	                                  const howdy::native::RuntimeConfig &config,
	                                  const char *username, const std::filesystem::path &models_dir,
	                                  const AuthenticationEligibilityDependencies &dependencies)
	    -> AuthenticationEligibilityResult;

}  // namespace howdy::pam::auth_eligibility
