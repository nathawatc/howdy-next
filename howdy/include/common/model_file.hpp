#pragma once

#include <filesystem>
#include <fstream>
#include <string>

namespace howdy::native {

    inline auto is_invalid_model_file(const std::filesystem::path &path) -> bool {
        if (!std::filesystem::is_regular_file(path)) {
            return true;
        }

        std::ifstream input(path, std::ios::binary);
        if (!input.is_open()) {
            return true;
        }

        std::string header(256, '\0');
        input.read(header.data(), static_cast<std::streamsize>(header.size()));
        header.resize(static_cast<std::size_t>(input.gcount()));
        return header.starts_with("version https://git-lfs.github.com/spec/v1") ||
               (!header.empty() && header.front() == '<');
    }

}  // namespace howdy::native
