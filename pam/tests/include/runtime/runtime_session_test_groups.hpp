#pragma once

auto RunRuntimeSessionFd3Probe(const char *marker_path) -> int;
auto RunRuntimeSessionSpawnTests() -> bool;
auto RunRuntimeSessionDeadlineTests() -> bool;
