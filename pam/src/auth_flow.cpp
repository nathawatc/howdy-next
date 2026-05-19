#include <cerrno>
#include <clocale>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
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
#include <optional>

#include <INIReader.h>

#include <security/pam_appl.h>
#include <security/pam_ext.h>
#include <security/pam_modules.h>

#include "enter_device.hpp"
#include "main.hpp"
#include "native_prompt_conversation.hpp"
#include "optional_task.hpp"
#include "prompt_workaround.hpp"
#include "status_mapping.hpp"
#include "common/file_security.hpp"
#include "common/user_names.hpp"
#include "config/config_utils.hpp"
#include <paths.hpp>

namespace {

constexpr auto kPromptRetryDelay =
    std::chrono::duration<int, std::chrono::milliseconds::period>(100);
constexpr int kMaxPromptRetries = 5;

auto S(const char *msg) -> const char * { return gettext(msg); }

auto make_wait_exit_status(int exit_code) -> int { return exit_code << 8; }

using ConversationFn = std::function<int(int, const char *)>;

struct RuntimeAuthFiles {
  bool active = false;
  std::filesystem::path root_dir;
  std::string config_path;
  std::string user_models_dir;

  ~RuntimeAuthFiles();
};

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

auto auth_token_present(pam_handle_t *pamh) -> bool {
  const void *auth_token = nullptr;
  const int result = pam_get_item(pamh, PAM_AUTHTOK, &auth_token);
  return result == PAM_SUCCESS && auth_token_item_present(auth_token);
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

auto check_enabled(const INIReader &config, const char *username,
                   const std::filesystem::path &user_models_dir) -> int {
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
      howdy::native::resolve_user_model_path(user_models_dir, username);
  if (!model_path) {
    syslog(LOG_WARNING, "Skipped authentication, invalid username");
    return PAM_AUTHINFO_UNAVAIL;
  }

  const auto models_dir_security =
      howdy::native::check_secure_root_owned_directory_tree(
          user_models_dir, "User models directory");
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

auto read_fd_to_string(int fd) -> std::string {
  std::string output;
  std::array<char, 1024> buffer {};
  while (true) {
    const ssize_t result = read(fd, buffer.data(), buffer.size());
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result <= 0) {
      break;
    }
    output.append(buffer.data(), static_cast<std::size_t>(result));
    if (output.size() > 8192) {
      break;
    }
  }
  return output;
}

auto helper_output_value(const std::string &output, const std::string &key)
    -> std::string {
  std::size_t offset = 0;
  while (offset < output.size()) {
    const auto next = output.find('\n', offset);
    const auto end = next == std::string::npos ? output.size() : next;
    const auto line = output.substr(offset, end - offset);
    const auto prefix = key + "=";
    if (line.rfind(prefix, 0) == 0) {
      return line.substr(prefix.size());
    }
    if (next == std::string::npos) {
      break;
    }
    offset = next + 1;
  }
  return {};
}

auto wait_for_helper_process(pid_t child_pid) -> int {
  while (true) {
    int status = 0;
    const pid_t wait_result = waitpid(child_pid, &status, 0);
    if (wait_result == child_pid) {
      return status;
    }
    if (wait_result < 0 && errno == EINTR) {
      continue;
    }
    return make_wait_exit_status(CompareError::ABORT);
  }
}

auto prepare_runtime_auth_files(const char *username, RuntimeAuthFiles *runtime)
    -> bool {
  std::array<int, 2> output_pipe = {-1, -1};
  if (pipe2(output_pipe.data(), O_CLOEXEC) != 0) {
    syslog(LOG_ERR, "Failed to create auth helper pipe: %s (%d)",
           strerror(errno), errno);
    return false;
  }

  posix_spawn_file_actions_t actions {};
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, output_pipe[0]);
  posix_spawn_file_actions_addclose(&actions, output_pipe[1]);

  std::array<char *, 4> args = {const_cast<char *>(AUTH_HELPER_PATH),
                                const_cast<char *>("prepare"),
                                const_cast<char *>(username), nullptr};
  std::array<char *, 1> env = {nullptr};
  pid_t child_pid = -1;
  const int spawn_result = posix_spawn(&child_pid, AUTH_HELPER_PATH, &actions,
                                       nullptr, args.data(), env.data());
  posix_spawn_file_actions_destroy(&actions);
  close(output_pipe[1]);

  if (spawn_result != 0) {
    close(output_pipe[0]);
    syslog(LOG_ERR, "Can't spawn the howdy auth helper: %s (%d)",
           strerror(spawn_result), spawn_result);
    return false;
  }

  const std::string helper_output = read_fd_to_string(output_pipe[0]);
  close(output_pipe[0]);

  const int status = wait_for_helper_process(child_pid);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS) {
    syslog(LOG_ERR, "Howdy auth helper failed: %s", helper_output.c_str());
    return false;
  }

  runtime->config_path = helper_output_value(helper_output, "CONFIG_PATH");
  runtime->user_models_dir =
      helper_output_value(helper_output, "USER_MODELS_DIR");
  if (runtime->config_path.empty() || runtime->user_models_dir.empty()) {
    syslog(LOG_ERR, "Howdy auth helper returned incomplete output: %s",
           helper_output.c_str());
    return false;
  }

  runtime->root_dir = std::filesystem::path(runtime->config_path).parent_path();
  runtime->active = true;
  return true;
}

auto cleanup_runtime_auth_files(const std::filesystem::path &root_dir) -> void {
  std::string root_dir_string = root_dir.string();
  std::array<char *, 4> args = {const_cast<char *>(AUTH_HELPER_PATH),
                                const_cast<char *>("cleanup"),
                                const_cast<char *>(root_dir_string.c_str()),
                                nullptr};
  std::array<char *, 1> env = {nullptr};
  pid_t child_pid = -1;
  const int spawn_result = posix_spawn(&child_pid, AUTH_HELPER_PATH, nullptr,
                                       nullptr, args.data(), env.data());
  if (spawn_result != 0) {
    syslog(LOG_WARNING, "Can't spawn the howdy auth helper cleanup: %s (%d)",
           strerror(spawn_result), spawn_result);
    return;
  }

  const int status = wait_for_helper_process(child_pid);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS) {
    syslog(LOG_WARNING, "Howdy auth helper cleanup failed");
  }
}

RuntimeAuthFiles::~RuntimeAuthFiles() {
  if (active && !root_dir.empty()) {
    cleanup_runtime_auth_files(root_dir);
  }
}

auto request_password_prompt_stop(optional_task<std::tuple<int, char *>> &pass_task,
                                  const PromptStopPlan &plan,
                                  NativePromptConversation *native_prompt)
    -> bool {
  if (!plan.stop_prompt) {
    return false;
  }

  bool enter_failed = false;
  if (plan.abort_prompt && native_prompt != nullptr) {
    native_prompt->request_abort();
  }

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

  pass_task.stop();
  return enter_failed;
}

}  // namespace

auto identify(pam_handle_t *pamh, int flags, int argc, const char **argv,
              bool ask_auth_tok) -> int {
  (void)flags;

  openlog("pam_howdy", 0, LOG_AUTHPRIV);

  char *username = nullptr;
  int pam_res = pam_get_user(pamh, const_cast<const char **>(&username), nullptr);
  if (pam_res != PAM_SUCCESS || username == nullptr || username[0] == '\0') {
    syslog(LOG_ERR, "Failed to get username");
    return pam_res == PAM_SUCCESS ? PAM_USER_UNKNOWN : pam_res;
  }

  RuntimeAuthFiles runtime_auth_files;
  std::string config_path = CONFIG_FILE_PATH;
  std::string user_models_dir = USER_MODELS_DIR;

  auto config_security = howdy::native::check_secure_config_path(config_path);
  if (!config_security.ok && config_security.error_code == EACCES &&
      geteuid() != 0) {
    if (!prepare_runtime_auth_files(username, &runtime_auth_files)) {
      return PAM_SYSTEM_ERR;
    }
    config_path = runtime_auth_files.config_path;
    user_models_dir = runtime_auth_files.user_models_dir;
    config_security = howdy::native::check_secure_config_path(config_path);
  }

  if (!config_security.ok) {
    syslog(LOG_ERR, "%s", config_security.error_message.c_str());
    return PAM_SYSTEM_ERR;
  }

  INIReader config(config_path);
  if (config.ParseError() != 0) {
    syslog(LOG_ERR, "Failed to parse the configuration file: %d",
           config.ParseError());
    return PAM_SYSTEM_ERR;
  }

  pam_res = check_enabled(config, username, user_models_dir);
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

  const Workaround workaround = get_pam_workaround(argc, argv);
  Workaround effective_workaround = workaround;
  const bool existing_auth_token = auth_token_present(pamh);

  std::array<char *, 5> args = {const_cast<char *>(COMPARE_PROCESS_PATH),
                                const_cast<char *>("--config"),
                                const_cast<char *>(config_path.c_str()),
                                username, nullptr};
  std::string user_models_env =
      "HOWDY_USER_MODELS_DIR=" + user_models_dir;
  std::array<char *, 2> runtime_env = {
      const_cast<char *>(user_models_env.c_str()), nullptr};
  std::array<char *, 1> empty_env = {nullptr};
  char **compare_env =
      runtime_auth_files.active ? runtime_env.data() : empty_env.data();
  pid_t child_pid = -1;

  const int spawn_result = posix_spawn(&child_pid, COMPARE_PROCESS_PATH, nullptr,
                                       nullptr, args.data(), compare_env);
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

  std::optional<NativePromptConversation> native_prompt;
  if (workaround == Workaround::Native && ask_auth_tok &&
      !existing_auth_token) {
    native_prompt.emplace(pamh);
    if (!native_prompt->available()) {
      syslog(LOG_INFO,
             "Native prompt conversation unavailable, falling back to input workaround");
      native_prompt.reset();
      effective_workaround = Workaround::Input;
    } else {
      const int install_result = native_prompt->install();
      if (install_result != PAM_SUCCESS) {
        syslog(LOG_WARNING, "Failed to install native prompt conversation: %d",
               install_result);
        native_prompt.reset();
        effective_workaround = Workaround::Input;
      }
    }
  }

  if (effective_workaround == Workaround::Input && ask_auth_tok &&
      !existing_auth_token &&
      euidaccess("/dev/uinput", W_OK | R_OK) != 0) {
    const int access_errno = errno;
    syslog(LOG_INFO,
           "Input prompt workaround unavailable, falling back to standard PAM prompt: %s (%d)",
           strerror(access_errno), access_errno);
    effective_workaround = Workaround::Off;
  }

  const bool ask_pass =
      effective_workaround == Workaround::Native
          ? native_prompt.has_value() && !existing_auth_token
          : should_ask_for_password(ask_auth_tok, effective_workaround,
                                    existing_auth_token);

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
    child_task.stop();
    if (ask_pass) {
      pass_task.stop();
      char *password = nullptr;
      std::tie(pam_res, password) = pass_task.get();
      (void)password;
      if (pam_res != PAM_SUCCESS) {
        return pam_res;
      }
    }
    return PAM_IGNORE;
  }

  child_task.stop();
  const int status = child_task.get();

  if (WIFEXITED(status) && WEXITSTATUS(status) != EXIT_SUCCESS && ask_pass) {
    pass_task.stop();

    char *password = nullptr;
    std::tie(pam_res, password) = pass_task.get();
    (void)password;
    if (pam_res != PAM_SUCCESS) {
      return howdy_status(username, status, config, conv_function);
    }

    return PAM_IGNORE;
  }

  const auto stop_plan =
      plan_prompt_stop(ask_pass, ask_pass && pass_task.ready(),
                       effective_workaround);
  const bool enter_failed =
      request_password_prompt_stop(pass_task, stop_plan,
                                   native_prompt ? &*native_prompt : nullptr);
  if (enter_failed) {
    send_conversation_message(
        conv_function, PAM_ERROR_MSG,
        S("Failed to send Enter press, waiting for user to press it instead"));
  }

  return howdy_status(username, status, config, conv_function);
}
