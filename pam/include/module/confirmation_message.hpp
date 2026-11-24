#pragma once

#include <string>
#include <string_view>

[[nodiscard]] auto BuildConfirmationMessage(std::string_view username) -> std::string;
