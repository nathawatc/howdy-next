#ifndef HOWDY_PAM_NATIVE_PROMPT_CONVERSATION_HPP
#define HOWDY_PAM_NATIVE_PROMPT_CONVERSATION_HPP

#include <array>
#include <atomic>

#ifdef HOWDY_PAM_TESTING
#include <csignal>
#endif

#include <security/pam_appl.h>

class NativePromptConversation {
public:
  explicit NativePromptConversation(pam_handle_t *pamh);
  ~NativePromptConversation();

  NativePromptConversation(const NativePromptConversation &) = delete;
  auto operator=(const NativePromptConversation &)
      -> NativePromptConversation & = delete;

  [[nodiscard]] auto available() const -> bool;
  auto install() -> int;
  void request_abort();

private:
  static auto dispatch(int num_msg, const struct pam_message **msgm,
                       struct pam_response **response, void *appdata_ptr)
      -> int;
  auto handle(int num_msg, const struct pam_message **msgm,
              struct pam_response **response) -> int;
  [[nodiscard]] auto write_message_line(const struct pam_message &message) const
      -> int;
  auto prompt_input(const struct pam_message &message, char **response,
                    bool hide_input) -> int;

  pam_handle_t *pamh_ = nullptr;
  struct pam_conv original_conv_ {};
  struct pam_conv override_conv_ {};
  bool has_original_conv_ = false;
  bool installed_ = false;
  int tty_fd_ = -1;
  std::array<int, 2> abort_pipe_{{-1, -1}};
  std::atomic<bool> abort_requested_{false};
};

#ifdef HOWDY_PAM_TESTING
class SigintAbortHandlerForTesting {
public:
  explicit SigintAbortHandlerForTesting(int abort_fd);
  ~SigintAbortHandlerForTesting();

  SigintAbortHandlerForTesting(const SigintAbortHandlerForTesting &) = delete;
  auto operator=(const SigintAbortHandlerForTesting &)
      -> SigintAbortHandlerForTesting & = delete;

  [[nodiscard]] auto installed() const -> bool;

private:
  struct sigaction previous_action_ {};
  bool installed_ = false;
};

void trigger_sigint_abort_for_testing();
auto sigint_abort_handler_ready_for_testing() -> bool;
#endif

#endif  // HOWDY_PAM_NATIVE_PROMPT_CONVERSATION_HPP
