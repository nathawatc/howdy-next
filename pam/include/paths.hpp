#pragma once

// Fallback path constants for source-only tooling (clangd/IDE diagnostics).
// Meson builds generate build/pam/paths.hpp and include that first via -Ipam.
const auto COMPARE_PROCESS_PATH = "/usr/local/lib/howdy/howdy-compare";
const auto CONFIG_FILE_PATH = "/usr/local/etc/howdy/config.ini";
const auto USER_MODELS_DIR = "/usr/local/etc/howdy/models";
