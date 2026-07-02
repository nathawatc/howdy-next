#pragma once

#ifdef HOWDY_PAM_TESTING

#	include "native_prompt_conversation.hpp"
#	include "optional_task.hpp"

#	include <tuple>

namespace howdy::pam::testing {

	void cleanup_native_prompt(optional_task<std::tuple<int, char *>> &pass_task,
	                           NativePromptConversation               &native_prompt) noexcept;

}  // namespace howdy::pam::testing

#endif
