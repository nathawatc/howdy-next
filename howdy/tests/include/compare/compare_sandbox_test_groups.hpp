#pragma once

#include <string_view>

auto RunCompareSandboxLimitTests() -> bool;
auto RunCompareSandboxSeccompTests() -> bool;
auto RunCompareSandboxSeccompProbe(std::string_view action) -> int;
