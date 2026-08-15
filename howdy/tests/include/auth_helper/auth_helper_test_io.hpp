#pragma once

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace howdy::test::auth_helper {

	inline auto write_file(const std::filesystem::path &path, const std::string &content) -> bool {
		std::ofstream output(path);
		output << content;
		return output.good();
	}

	inline auto read_file(const std::filesystem::path &path) -> std::string {
		std::ifstream input(path);
		return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	}

}  // namespace howdy::test::auth_helper
