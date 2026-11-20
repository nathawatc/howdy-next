#pragma once

#include <filesystem>

auto RunAuthHelperDiagnosticsTests() -> bool;
auto RunAuthHelperDescriptorTests(const std::filesystem::path &temp_root) -> bool;
