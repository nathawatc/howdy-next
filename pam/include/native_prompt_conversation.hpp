#ifndef HOWDY_PAM_NATIVE_PROMPT_CONVERSATION_HPP
#define HOWDY_PAM_NATIVE_PROMPT_CONVERSATION_HPP

#include <array>
#include <atomic>

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
  auto delegate(int num_msg, const struct pam_message **msgm,
                struct pam_response **response) const -> int;
  auto prompt_hidden_password(const struct pam_message &message,
                              struct pam_response **response) -> int;

  pam_handle_t *pamh_ = nullptr;
  struct pam_conv original_conv_ {};
  struct pam_conv override_conv_ {};
  bool has_original_conv_ = false;
  bool installed_ = false;
  int tty_fd_ = -1;
  std::array<int, 2> abort_pipe_{{-1, -1}};
  std::atomic<bool> abort_requested_{false};
};

#endif  // HOWDY_PAM_NATIVE_PROMPT_CONVERSATION_HPP
