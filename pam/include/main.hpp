#ifndef MAIN_H_
#define MAIN_H_

#include <cstring>
#include <security/pam_modules.h>
#include <string>
#include <string_view>
#include <cstdint>
#include <unistd.h>

enum class ConfirmationType : std::uint8_t { Unset, Howdy, Pam };
enum class Workaround : std::uint8_t { Off, Input, Native };

// Exit status codes returned by the compare process
enum CompareError : std::uint8_t {
  NO_FACE_MODEL = 10,
  TIMEOUT_REACHED = 11,
  ABORT = 12,
  TOO_DARK = 13,
  INVALID_DEVICE = 14,
  RUBBERSTAMP = 15
};

inline auto get_workaround(std::string_view workaround) -> Workaround {
  if (workaround == "input") {
    return Workaround::Input;
  }

  if (workaround == "native") {
    return Workaround::Native;
  }

  return Workaround::Off;
}

inline auto get_pam_workaround(int argc, const char *const *argv) -> Workaround {
  if (argv == nullptr) {
    return Workaround::Off;
  }

  constexpr std::string_view kPrefix = "workaround=";
  for (int index = 0; index < argc; ++index) {
    if (argv[index] == nullptr) {
      continue;
    }
    const std::string_view argument(argv[index]);
    if (argument.rfind(kPrefix, 0) == 0) {
      return get_workaround(argument.substr(kPrefix.size()));
    }
  }
  return Workaround::Off;
}

/**
 * Check if an environment variable exists either in the environ array or using
 * getenv.
 * @param name The name of the environment variable.
 * @return The value of the environment variable or nullptr if it doesn't exist
 * or environ is nullptr.
 * @note This function was created because `getenv` wasn't working properly in
 * some contexts (like sudo).
 */
inline auto checkenv(const char *name) -> bool {
  if (std::getenv(name) != nullptr) {
    return true;
  }

  if (environ == nullptr) {
    return false;
  }

  const auto len = strlen(name);

  for (char **env = environ; *env != nullptr; env++) {
    if (strncmp(*env, name, len) == 0 && (*env)[len] == '=') {
      return true;
    }
  }

  return false;
}

inline auto auth_token_item_present(const void *auth_token) -> bool {
  return auth_token != nullptr;
}

auto identify(pam_handle_t *pamh, int flags, int argc, const char **argv,
              bool ask_auth_tok) -> int;

#endif // MAIN_H_
