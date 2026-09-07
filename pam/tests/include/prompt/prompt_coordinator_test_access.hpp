#pragma once

#include "prompt/prompt_coordinator.hpp"

#include <chrono>
#include <memory>
#include <mutex>
#include <utility>

namespace howdy::pam {
	class PromptCoordinatorTestAccess {
	public:
		static auto WaitForCompareSuccess(PromptCoordinator                  &coordinator,
		                                  std::chrono::steady_clock::duration timeout) -> bool {
			std::unique_lock<std::mutex> lock(coordinator.mutex_);
			return coordinator.condition_.wait_for(lock, timeout, [&coordinator] -> bool {
				return coordinator.state_.compare_succeeded;
			});
		}

		[[nodiscard]] static auto PasswordCallReturned(PromptCoordinator &coordinator) -> bool {
			std::scoped_lock lock(coordinator.mutex_);
			return coordinator.state_.password_call_returned;
		}

		static auto WaitForPasswordCallReturned(PromptCoordinator                  &coordinator,
		                                        std::chrono::steady_clock::duration timeout)
		    -> bool {
			std::unique_lock<std::mutex> lock(coordinator.mutex_);
			return coordinator.condition_.wait_for(lock, timeout, [&coordinator] -> bool {
				return coordinator.state_.password_call_returned;
			});
		}

		static void RequestShutdown(PromptCoordinator &coordinator) {
			{
				std::scoped_lock lock(coordinator.mutex_);
				coordinator.state_.shutdown_requested     = true;
				coordinator.state_.cancellation_requested = true;
				if (coordinator.state_.submission ==
				    PromptCoordinator::PromptSubmissionState::kClaimed) {
					coordinator.state_.submission =
					    PromptCoordinator::PromptSubmissionState::kPending;
					coordinator.state_.claimed_generation = 0;
				}
			}
			coordinator.condition_.notify_all();
		}

		static void PrepareClaimedSubmission(PromptCoordinator               &coordinator,
		                                     std::unique_ptr<PromptSubmitter> prompt_submitter) {
			std::scoped_lock lock(coordinator.mutex_);
			coordinator.prompt_submitter_            = std::move(prompt_submitter);
			coordinator.state_.first_completion      = PromptCoordinator::FirstCompletion::kCompare;
			coordinator.state_.compare_succeeded     = true;
			coordinator.state_.password_call_entered = true;
			coordinator.state_.secret_prompt_generation = 1;
			coordinator.state_.claimed_generation       = 1;
			coordinator.state_.secret_prompt_active     = true;
			coordinator.state_.submission = PromptCoordinator::PromptSubmissionState::kClaimed;
		}

		static auto PromptSubmissionFinished(PromptCoordinator &coordinator) -> bool {
			std::scoped_lock lock(coordinator.mutex_);
			return coordinator.state_.submission ==
			       PromptCoordinator::PromptSubmissionState::kFinished;
		}

		static void PublishPasswordCallReturned(PromptCoordinator &coordinator) {
			coordinator.PublishPasswordCallReturned();
		}

		static void ClosePromptGeneration(PromptCoordinator     &coordinator,
		                                  SecretPromptGeneration generation) {
			PromptCoordinator::SecretPromptEnd(&coordinator, generation);
		}

		static auto BeginPromptGeneration(PromptCoordinator &coordinator)
		    -> SecretPromptGeneration {
			return PromptCoordinator::SecretPromptBegin(&coordinator);
		}

		static void SubmitPromptForGenerations(PromptCoordinator &coordinator) {
			coordinator.SubmitPromptForGenerations();
		}
	};
}  // namespace howdy::pam
