#pragma once

#include <memory>

namespace howdy::pam {
	class PromptSubmitter {
	public:
		PromptSubmitter()                                            = default;
		PromptSubmitter(const PromptSubmitter &)                     = delete;
		auto operator=(const PromptSubmitter &) -> PromptSubmitter & = delete;
		PromptSubmitter(PromptSubmitter &&)                          = delete;
		auto operator=(PromptSubmitter &&) -> PromptSubmitter &      = delete;
		virtual ~PromptSubmitter()                                   = default;

		virtual void SubmitPrompt() = 0;
	};

	auto CreateUinputPromptSubmitter() -> std::unique_ptr<PromptSubmitter>;

}  // namespace howdy::pam
