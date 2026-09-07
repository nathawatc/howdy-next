#ifndef HOWDY_SRC_PAM_PROMPT_SUBMITTER_HH
#define HOWDY_SRC_PAM_PROMPT_SUBMITTER_HH

#include <memory>

namespace howdy::pam {
	class PromptSubmitter {
	public:
		virtual ~PromptSubmitter() = default;

		virtual void SubmitPrompt() = 0;
	};

	auto CreateUinputPromptSubmitter() -> std::unique_ptr<PromptSubmitter>;

}  // namespace howdy::pam

#endif  // HOWDY_SRC_PAM_PROMPT_SUBMITTER_HH
