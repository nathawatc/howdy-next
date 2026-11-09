#pragma once

#include "../../../src/config/config_document/internal.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace howdy::native::test_support {

	// Test-only file loader. Production code reads config through the secure fd paths
	// (ownership, mode, size), never by path.
	inline auto ReadConfigDocument(const std::filesystem::path &path) -> ConfigDocument {
		std::ifstream input(path, std::ios::binary);
		if (!input) {
			// A missing fixture is a broken test, not a config error to assert on.
			std::cerr << "cannot open test config " << path.string() << "\n";
			std::abort();
		}
		const std::string content{std::istreambuf_iterator<char>(input),
		                          std::istreambuf_iterator<char>()};
		return ConfigDocument::FromContent(content);
	}

}  // namespace howdy::native::test_support
