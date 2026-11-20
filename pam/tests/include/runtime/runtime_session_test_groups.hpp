#pragma once

#include <string_view>

// Child modes of the test binary, which stands in for the auth helper.
auto RunFakeHelper(std::string_view variant) -> int;
auto RunRuntimeSessionFd3Probe(const char *marker_path) -> int;
auto RunStalledHelper(const char *marker_path) -> int;

auto RunRuntimeSessionSpawnTests() -> bool;
auto RunRuntimeSessionDeadlineTests() -> bool;
