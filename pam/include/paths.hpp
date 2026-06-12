#pragma once

// Fallback path constants for source-only tooling (clangd/IDE diagnostics).
// Meson builds generate build/pam/paths.hpp and include that first via -Ipam.
inline constexpr auto kDefaultDevConfigPath    = "/usr/local/etc/howdy/config.ini";
inline constexpr auto kConfiguredConfigPath    = "/usr/local/etc/howdy/config.ini";
inline constexpr auto kConfiguredModelsDir     = "/usr/local/share/howdy/models";
inline constexpr auto kConfiguredUserModelsDir = "/usr/local/etc/howdy/models";
inline constexpr auto kConfiguredLogPath       = "/var/log/howdy";
inline constexpr auto kCompareProcessPath      = "/usr/local/lib/howdy/howdy-compare";
inline constexpr auto kAuthHelperPath          = "/usr/local/lib/howdy/howdy-auth-helper";
