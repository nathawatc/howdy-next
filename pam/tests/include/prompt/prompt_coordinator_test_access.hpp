#pragma once

#include "prompt/prompt_coordinator.hpp"

#include <chrono>
#include <memory>
#include <mutex>
#include <utility>

namespace howdy::pam {
	class PromptCoordinatorTestAccess {
	public:
		static auto wait_for_compare_success(PromptCoordinator                  &coordinator,
		                                     std::chrono::steady_clock::duration timeout) -> bool {
			std::unique_lock<std::mutex> lock(coordinator.mutex_);
			return coordinator.condition_.wait_for(lock, timeout, [&coordinator] -> bool {
				return coordinator.state_.compare_succeeded;
			});
		}

		[[nodiscard]] static auto password_call_returned(PromptCoordinator &coordinator) -> bool {
			std::scoped_lock lock(coordinator.mutex_);
			return coordinator.state_.password_call_returned;
		}

		static auto wait_for_password_call_returned(PromptCoordinator                  &coordinator,
		                                            std::chrono::steady_clock::duration timeout)
		    -> bool {
			std::unique_lock<std::mutex> lock(coordinator.mutex_);
			return coordinator.condition_.wait_for(lock, timeout, [&coordinator] -> bool {
				return coordinator.state_.password_call_returned;
			});
		}

		static void request_shutdown(PromptCoordinator &coordinator) {
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

		static void prepare_claimed_submission(PromptCoordinator               &coordinator,
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

		static auto prompt_submission_finished(PromptCoordinator &coordinator) -> bool {
			std::scoped_lock lock(coordinator.mutex_);
			return coordinator.state_.submission ==
			       PromptCoordinator::PromptSubmissionState::kFinished;
		}

		static void publish_password_call_returned(PromptCoordinator &coordinator) {
			coordinator.publish_password_call_returned();
		}

		static void close_prompt_generation(PromptCoordinator     &coordinator,
		                                    SecretPromptGeneration generation) {
			PromptCoordinator::secret_prompt_end(&coordinator, generation);
		}

		static auto begin_prompt_generation(PromptCoordinator &coordinator)
		    -> SecretPromptGeneration {
			return PromptCoordinator::secret_prompt_begin(&coordinator);
		}

		static void submit_prompt_for_generations(PromptCoordinator &coordinator) {
			coordinator.submit_prompt_for_generations();
		}
	};
}  // namespace howdy::pam
