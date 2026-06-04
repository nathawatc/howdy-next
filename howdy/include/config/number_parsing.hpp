#pragma once

#include <charconv>
#include <cmath>
#include <optional>
#include <string_view>
#include <system_error>

namespace howdy::native {

    inline auto parse_config_float_strict(std::string_view value) -> std::optional<float> {
        if (value.empty()) {
            return std::nullopt;
        }

        const char *first = value.data();
        const char *last  = value.data() + value.size();
        if (*first == '+') {
            ++first;
            if (first == last) {
                return std::nullopt;
            }
        }

        float      parsed = 0.0F;
        const auto result = std::from_chars(first, last, parsed);
        if (result.ec != std::errc{} || result.ptr != last || !std::isfinite(parsed)) {
            return std::nullopt;
        }

        return parsed;
    }

}  // namespace howdy::native
