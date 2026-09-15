howdy_add_pam_test(
	pam_prompt_workaround_test
	pam-prompt-workaround
	tests/prompt/prompt_workaround_test.cpp
)
target_link_libraries(pam_prompt_workaround_test PRIVATE pam_prompt_core)

howdy_add_pam_test(
	pam_internal_fd_test
	pam-internal-fd
	tests/prompt/internal_fd_test.cpp
)
target_link_libraries(pam_internal_fd_test PRIVATE pam_prompt)

howdy_add_pam_test(
	pam_native_prompt_conversation_test
	pam-native-prompt-conversation
	tests/prompt/native_prompt_conversation_test.cpp
	tests/prompt/native_prompt_terminal_test.cpp
	tests/prompt/native_prompt_fd_test.cpp
	tests/prompt/native_prompt_input_test.cpp
)
target_link_libraries(pam_native_prompt_conversation_test PRIVATE pam_prompt)

howdy_add_pam_test(
	pam_observed_prompt_conversation_test
	pam-observed-prompt-conversation
	tests/prompt/observed_prompt_conversation_test.cpp
)
target_link_libraries(pam_observed_prompt_conversation_test PRIVATE pam_prompt)

howdy_add_pam_test(
	pam_conversation_response_test
	pam-conversation-response
	tests/prompt/conversation_response_test.cpp
)
target_link_libraries(pam_conversation_response_test PRIVATE
                      pam_conversation_response)

howdy_add_pam_test(
	pam_conversation_test
	pam-conversation
	tests/prompt/pam_conversation_test.cpp
)
target_link_libraries(pam_conversation_test PRIVATE pam_prompt)

howdy_add_pam_test(
	pam_prompt_coordinator_test
	pam-prompt-coordinator
	tests/prompt/prompt_coordinator_test.cpp
	tests/prompt/prompt_coordinator_modes_test.cpp
	tests/prompt/prompt_coordinator_adapter_test.cpp
	src/prompt/observed_prompt_conversation.cpp
)
target_link_libraries(
	pam_prompt_coordinator_test
	PRIVATE pam_prompt_core pam_conversation_response
)

howdy_add_pam_test(
	pam_prompt_coordinator_runtime_test
	pam-prompt-coordinator-runtime
	tests/prompt/prompt_coordinator_runtime_test.cpp
)
target_link_libraries(
	pam_prompt_coordinator_runtime_test
	PRIVATE pam_prompt_core pam_runtime
)
