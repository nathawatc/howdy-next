#pragma once

#include "config/runtime_config.hpp"
#include "runtime/lid_probe.hpp"
#include "storage/user_model_readiness.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
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

	using SshSessionPresentFn   = bool (*)(void *context, pam_handle_t *pamh);
	using ReadLidStateFn        = howdy::pam::runtime::LidStateResult (*)(void *context);
	using CheckModelReadinessFn = howdy::native::UserModelReadinessResult (*)(
	    void *context, const std::filesystem::path &models_dir, const char *username);

	class AuthenticationEligibilityOperations {
	public:
		static auto Create(void *context, SshSessionPresentFn ssh_session_present,
		                   ReadLidStateFn        read_lid_state,
		                   CheckModelReadinessFn check_model_readiness)
		    -> std::optional<AuthenticationEligibilityOperations> {
			if (ssh_session_present == nullptr || read_lid_state == nullptr ||
			    check_model_readiness == nullptr) {
				return std::nullopt;
			}
			return AuthenticationEligibilityOperations(context, ssh_session_present, read_lid_state,
			                                           check_model_readiness);
		}

		[[nodiscard]] auto SshSessionPresent(pam_handle_t *pamh) const -> bool {
			return ssh_session_present_(context_, pamh);
		}

		[[nodiscard]] auto ReadLidState() const -> howdy::pam::runtime::LidStateResult {
			return read_lid_state_(context_);
		}

		[[nodiscard]] auto CheckModelReadiness(const std::filesystem::path &models_dir,
		                                       const char                  *username) const
		    -> howdy::native::UserModelReadinessResult {
			return check_model_readiness_(context_, models_dir, username);
		}

	private:
		AuthenticationEligibilityOperations(void *context, SshSessionPresentFn ssh_session_present,
		                                    ReadLidStateFn        read_lid_state,
		                                    CheckModelReadinessFn check_model_readiness)
		    : context_(context)
		    , ssh_session_present_(ssh_session_present)
		    , read_lid_state_(read_lid_state)
		    , check_model_readiness_(check_model_readiness) {}

		void                 *context_               = nullptr;
		SshSessionPresentFn   ssh_session_present_   = nullptr;
		ReadLidStateFn        read_lid_state_        = nullptr;
		CheckModelReadinessFn check_model_readiness_ = nullptr;
	};

	auto ClassifyModelReadiness(const howdy::native::UserModelReadinessResult &readiness)
	    -> ModelCondition;

	auto ProductionAuthenticationEligibilityOperations() -> AuthenticationEligibilityOperations;

	auto DecideAuthenticationEligibility(const howdy::native::RuntimeConfig &config,
	                                     const AuthenticationConditions     &conditions)
	    -> AuthenticationEligibilityResult;

	auto EvaluateAuthenticationEligibility(pam_handle_t                              *pamh,
	                                       const howdy::native::RuntimeConfig        &config,
	                                       const char                                *username,
	                                       const std::filesystem::path               &models_dir,
	                                       const AuthenticationEligibilityOperations &operations)
	    -> AuthenticationEligibilityResult;

}  // namespace howdy::pam::auth_eligibility
