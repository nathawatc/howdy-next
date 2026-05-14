#include <cerrno>
#include <clocale>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <glob.h>
#include <libintl.h>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <tuple>
#include <unistd.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>

#include <INIReader.h>

#include <security/pam_appl.h>
#include <security/pam_ext.h>
#include <security/pam_modules.h>

#include "enter_device.hh"
#include "main.hh"
#include "optional_task.hh"
#include "prompt_workaround.hh"
#include "status_mapping.hh"
#include "tty_restore.hh"
#include "common/file_security.hpp"
#include "common/user_names.hpp"
#include "config/config_utils.hpp"
#include <paths.hh>

namespace {

constexpr auto kPromptRetryDelay =
    std::chrono::duration<int, std::chrono::milliseconds::period>(100);
constexpr int kMaxPromptRetries = 5;

auto S(const char *msg) -> const char * { return gettext(msg); }

auto make_wait_exit_status(int exit_code) -> int { return exit_code << 8; }

using ConversationFn = std::function<int(int, const char *)>;

auto send_conversation_message(const ConversationFn &conv_function,
                               int msg_type,
                               const std::string &message) -> void {
  const int result = conv_function(msg_type, message.c_str());
  if (result != PAM_SUCCESS) {
    syslog(LOG_WARNING, "Failed to send PAM conversation message: %d", result);
  }
}

auto make_conversation(pam_handle_t *pamh, ConversationFn *conv_function)
    -> int {
  struct pam_conv *conv = nullptr;
  const void **conv_ptr =
      const_cast<const void **>(reinterpret_cast<void **>(&conv));
  const int pam_res = pam_get_item(pamh, PAM_CONV, conv_ptr);
  if (pam_res != PAM_SUCCESS) {
    syslog(LOG_ERR, "Failed to acquire conversation");
    return pam_res;
  }

  if (conv == nullptr || conv->conv == nullptr) {
    syslog(LOG_ERR, "PAM conversation is not available");
    return PAM_SYSTEM_ERR;
  }

  *conv_function = [conv](int msg_type, const char *msg_str) {
    const struct pam_message msg = {.msg_style = msg_type, .msg = msg_str};
    const struct pam_message *msgp = &msg;
    struct pam_response *resp = nullptr;
    const int conv_result = conv->conv(1, &msgp, &resp, conv->appdata_ptr);
    if (resp != nullptr) {
      if (resp->resp != nullptr) {
        std::memset(resp->resp, 0, std::strlen(resp->resp));
        std::free(resp->resp);
      }
      std::free(resp);
    }
    return conv_result;
  };

  return PAM_SUCCESS;
}

auto howdy_error(int status, const ConversationFn &conv_function) -> int {
  const auto decision = map_compare_wait_status(status);
  if (decision.conversation_kind == ConversationKind::Error) {
    send_conversation_message(conv_function, PAM_ERROR_MSG,
                              decision.conversation_message);
  } else if (decision.conversation_kind == ConversationKind::Info) {
    send_conversation_message(conv_function, PAM_TEXT_INFO,
                              decision.conversation_message);
  }

  if (WIFEXITED(status) && WEXITSTATUS(status) == CompareError::NO_FACE_MODEL) {
    syslog(LOG_NOTICE, "%s", decision.log_message.c_str());
  } else if (WIFEXITED(status) &&
             WEXITSTATUS(status) != CompareError::NO_FACE_MODEL) {
    syslog(LOG_ERR, "%s", decision.log_message.c_str());
  } else if (WIFSIGNALED(status)) {
    syslog(LOG_ERR, "%s (%d)", decision.log_message.c_str(), WTERMSIG(status));
  }

  return PAM_AUTH_ERR;
}

auto howdy_status(char *username, int status, const INIReader &config,
                  const ConversationFn &conv_function) -> int {
  if (status != EXIT_SUCCESS) {
    return howdy_error(status, conv_function);
  }

  if (!config.GetBoolean("core", "no_confirmation", true)) {
    send_conversation_message(conv_function, PAM_TEXT_INFO,
                              build_confirmation_message(username));
  }

  syslog(LOG_INFO, "Login approved");
  return PAM_SUCCESS;
}

auto check_enabled(const INIReader &config, const char *username) -> int {
  if (config.GetBoolean("core", "disabled", false)) {
    syslog(LOG_INFO, "Skipped authentication, Howdy is disabled");
    return PAM_AUTHINFO_UNAVAIL;
  }

  if (config.GetBoolean("core", "abort_if_ssh", true)) {
    if (checkenv("SSH_CONNECTION") || checkenv("SSH_CLIENT") ||
        checkenv("SSH_TTY") || checkenv("SSHD_OPTS")) {
      syslog(LOG_INFO, "Skipped authentication, SSH session detected");
      return PAM_AUTHINFO_UNAVAIL;
    }
  }

  if (config.GetBoolean("core", "abort_if_lid_closed", true)) {
    glob_t glob_result {};
    const int return_value =
        glob("/proc/acpi/button/lid/*/state", 0, nullptr, &glob_result);

    if (return_value != 0 && return_value != GLOB_NOMATCH) {
      syslog(LOG_ERR, "Failed to read files from glob: %d", return_value);
      if (errno != 0) {
        syslog(LOG_ERR, "Underlying error: %s (%d)", strerror(errno), errno);
      }
    } else {
      for (size_t i = 0; i < glob_result.gl_pathc; i++) {
        std::ifstream file(std::string(glob_result.gl_pathv[i]));
        std::string lid_state;
        std::getline(file, lid_state);

        if (lid_state.find("closed") != std::string::npos) {
          globfree(&glob_result);
          syslog(LOG_INFO, "Skipped authentication, closed lid detected");
          return PAM_AUTHINFO_UNAVAIL;
        }
      }
    }
    globfree(&glob_result);
  }

  const auto model_path =
      howdy::native::resolve_user_model_path(USER_MODELS_DIR, username);
  if (!model_path) {
    syslog(LOG_WARNING, "Skipped authentication, invalid username");
    return PAM_AUTHINFO_UNAVAIL;
  }

  const auto models_dir_security =
      howdy::native::check_secure_root_owned_directory_tree(
          USER_MODELS_DIR, "User models directory");
  if (!models_dir_security.ok) {
    syslog(LOG_ERR, "%s", models_dir_security.error_message.c_str());
    return PAM_AUTHINFO_UNAVAIL;
  }

  struct stat stat_ {};
  if (lstat(model_path->c_str(), &stat_) != 0) {
    return PAM_AUTHINFO_UNAVAIL;
  }

  const auto model_file_security =
      howdy::native::check_secure_root_owned_file_with_directory(
          *model_path, "User models directory", "User model file");
  if (!model_file_security.ok) {
    syslog(LOG_ERR, "%s", model_file_security.error_message.c_str());
    return PAM_AUTHINFO_UNAVAIL;
  }

  return PAM_SUCCESS;
}

auto wait_for_compare_process(pid_t child_pid) -> int {
  while (true) {
    int status = 0;
    const pid_t wait_result = waitpid(child_pid, &status, 0);
    if (wait_result == child_pid) {
      return status;
    }
    if (wait_result < 0 && errno == EINTR) {
      continue;
    }

    syslog(LOG_ERR, "waitpid failed for compare process: %s (%d)",
           strerror(errno), errno);
    return make_wait_exit_status(CompareError::ABORT);
  }
}

auto request_password_prompt_stop(optional_task<std::tuple<int, char *>> &pass_task,
                                  const PromptStopPlan &plan) -> bool {
  if (!plan.stop_prompt) {
    return false;
  }

  bool enter_failed = false;
  if (plan.send_enter) {
    if (euidaccess("/dev/uinput", W_OK | R_OK) != 0) {
      syslog(LOG_WARNING, "Insufficient permissions to create the fake device");
      enter_failed = true;
    } else {
      try {
        EnterDevice enter_device;
        enter_device.send_enter_press();

        int retries = 0;
        for (; retries < kMaxPromptRetries &&
               pass_task.wait(kPromptRetryDelay) == std::future_status::timeout;
             retries++) {
          enter_device.send_enter_press();
        }

        if (retries == kMaxPromptRetries) {
          syslog(LOG_WARNING,
                 "Failed to send enter input before the retries limit");
          enter_failed = true;
        }
      } catch (const std::runtime_error &err) {
        syslog(LOG_WARNING, "Failed to send enter input: %s", err.what());
        enter_failed = true;
      }
    }
  }

  pass_task.stop(plan.force_cancel);
  return enter_failed;
}

}  // namespace

auto identify(pam_handle_t *pamh, int flags, int argc, const char **argv,
              bool ask_auth_tok) -> int {
  (void)flags;
  (void)argc;
  (void)argv;

  const auto config_security =
      howdy::native::check_secure_config_path(CONFIG_FILE_PATH);
  openlog("pam_howdy", 0, LOG_AUTHPRIV);
  if (!config_security.ok) {
    syslog(LOG_ERR, "%s", config_security.error_message.c_str());
    return PAM_SYSTEM_ERR;
  }

  INIReader config(CONFIG_FILE_PATH);
  if (config.ParseError() != 0) {
    syslog(LOG_ERR, "Failed to parse the configuration file: %d",
           config.ParseError());
    return PAM_SYSTEM_ERR;
  }

  char *username = nullptr;
  int pam_res = pam_get_user(pamh, const_cast<const char **>(&username), nullptr);
  if (pam_res != PAM_SUCCESS || username == nullptr || username[0] == '\0') {
    syslog(LOG_ERR, "Failed to get username");
    return pam_res == PAM_SUCCESS ? PAM_USER_UNKNOWN : pam_res;
  }

  pam_res = check_enabled(config, username);
  if (pam_res != PAM_SUCCESS) {
    return pam_res;
  }

  ConversationFn conv_function;
  pam_res = make_conversation(pamh, &conv_function);
  if (pam_res != PAM_SUCCESS) {
    return pam_res;
  }

  setlocale(LC_ALL, "");
  bindtextdomain(GETTEXT_PACKAGE, LOCALEDIR);
  textdomain(GETTEXT_PACKAGE);

  if (config.GetBoolean("core", "detection_notice", true)) {
    const int notice_result =
        conv_function(PAM_TEXT_INFO, S("Attempting facial authentication"));
    if (notice_result != PAM_SUCCESS) {
      syslog(LOG_ERR, "Failed to send detection notice");
    }
  }

  const Workaround workaround =
      get_workaround(config.GetString("core", "workaround", "input"));
  const bool ask_pass = should_ask_for_password(ask_auth_tok, workaround);

  std::array<char *, 3> args = {const_cast<char *>(COMPARE_PROCESS_PATH),
                                username, nullptr};
  std::array<char *, 1> env = {nullptr};
  pid_t child_pid = -1;

  const int spawn_result = posix_spawn(&child_pid, COMPARE_PROCESS_PATH, nullptr,
                                       nullptr, args.data(), env.data());
  if (spawn_result != 0) {
    syslog(LOG_ERR, "Can't spawn the howdy process: %s (%d)",
           strerror(spawn_result), spawn_result);
    return PAM_SYSTEM_ERR;
  }

  std::mutex mutx;
  std::condition_variable convar;
  ConfirmationType confirmation_type(ConfirmationType::Unset);

  optional_task<int> child_task([&] {
    const int status = wait_for_compare_process(child_pid);
    {
      std::unique_lock<std::mutex> lock(mutx);
      if (confirmation_type == ConfirmationType::Unset) {
        confirmation_type = ConfirmationType::Howdy;
      }
    }
    convar.notify_one();
    return status;
  });
  child_task.activate();

  TtyRestoreContext tty_restore(pamh);
  optional_task<std::tuple<int, char *>> pass_task([&] {
    char *auth_tok_ptr = nullptr;
    const int auth_result = pam_get_authtok(
        pamh, PAM_AUTHTOK, const_cast<const char **>(&auth_tok_ptr), nullptr);
    {
      std::unique_lock<std::mutex> lock(mutx);
      if (confirmation_type == ConfirmationType::Unset) {
        confirmation_type = ConfirmationType::Pam;
      }
    }
    convar.notify_one();
    return std::tuple<int, char *>(auth_result, auth_tok_ptr);
  });

  if (ask_pass) {
    pass_task.activate();
  }

  {
    std::unique_lock<std::mutex> lock(mutx);
    convar.wait(lock,
                [&] { return confirmation_type != ConfirmationType::Unset; });
  }

  if (confirmation_type == ConfirmationType::Pam) {
    if (kill(child_pid, SIGTERM) != 0 && errno != ESRCH) {
      syslog(LOG_WARNING, "Failed to terminate compare process: %s (%d)",
             strerror(errno), errno);
    }
    child_task.stop(false);
    if (ask_pass) {
      pass_task.stop(false);
      char *password = nullptr;
      std::tie(pam_res, password) = pass_task.get();
      (void)password;
      if (pam_res != PAM_SUCCESS) {
        return pam_res;
      }
    }
    return PAM_IGNORE;
  }

  child_task.stop(false);
  const int status = child_task.get();

  if (WIFEXITED(status) && WEXITSTATUS(status) != EXIT_SUCCESS && ask_pass) {
    pass_task.stop(false);

    char *password = nullptr;
    std::tie(pam_res, password) = pass_task.get();
    (void)password;
    if (pam_res != PAM_SUCCESS) {
      return howdy_status(username, status, config, conv_function);
    }

    return PAM_IGNORE;
  }

  const auto stop_plan =
      plan_prompt_stop(ask_pass, ask_pass && pass_task.ready(), workaround);
  const bool enter_failed = request_password_prompt_stop(pass_task, stop_plan);
  if (enter_failed) {
    send_conversation_message(
        conv_function, PAM_ERROR_MSG,
        S("Failed to send Enter press, waiting for user to press it instead"));
  }

  if (stop_plan.force_cancel && tty_restore.can_restore()) {
    std::string error_message;
    if (!tty_restore.restore_echo(&error_message)) {
      syslog(LOG_WARNING, "%s", error_message.c_str());
    }
  }

  return howdy_status(username, status, config, conv_function);
}
