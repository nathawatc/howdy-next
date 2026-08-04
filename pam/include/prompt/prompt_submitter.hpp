#ifndef HOWDY_SRC_PAM_PROMPT_SUBMITTER_HH
#define HOWDY_SRC_PAM_PROMPT_SUBMITTER_HH

#include <memory>

namespace howdy::pam {
	class PromptSubmitter {
	public:
		virtual ~PromptSubmitter() = default;

		virtual void submit_prompt() = 0;
	};

	auto create_uinput_prompt_submitter() -> std::unique_ptr<PromptSubmitter>;

}  // namespace howdy::pam

#endif  // HOWDY_SRC_PAM_PROMPT_SUBMITTER_HH
