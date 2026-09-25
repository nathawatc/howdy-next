#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
	// The staging tree must be exclusively controlled by the installer until the
	// install finishes. Preflight cannot protect CMake's later path-based writes
	// against concurrent renames or mount changes.
	const auto kCheckPath = [](const fs::path &path, bool directory) -> void {
		fs::path current;
		for (const auto &part : path) {
			current /= part;
			const auto status = fs::symlink_status(current);
			if (status.type() == fs::file_type::not_found) {
				continue;
			}
			const bool leaf = current == path && !directory;
			if (leaf ? (!fs::is_regular_file(status) || fs::hard_link_count(current) != 1)
			         : !fs::is_directory(status)) {
				throw std::runtime_error("Unsafe install destination: " + current.string());
			}
		}
	};

}  // namespace

auto main(int argc, char **argv) -> int {
	try {
		if (argc != 3) {
			throw std::runtime_error("usage: install_preflight DESTDIR MANIFEST");
		}
		const auto                     input = std::span(argv, static_cast<std::size_t>(argc));
		const std::vector<std::string> arguments(input.begin(), input.end());
		const fs::path                 stage(arguments.at(1));
		if (!stage.is_absolute()) {
			throw std::runtime_error("DESTDIR must be absolute");
		}
		for (const auto &part : stage) {
			if (part == "..") {
				throw std::runtime_error("DESTDIR must not contain '..'");
			}
		}
		kCheckPath(stage, true);
		std::ifstream manifest(arguments.at(2));
		if (!manifest) {
			throw std::runtime_error("Cannot read install destination manifest");
		}
		std::string entry;
		while (std::getline(manifest, entry)) {
			if (entry.size() < 2 || (entry.front() != 'F' && entry.front() != 'D')) {
				throw std::runtime_error("Invalid install destination manifest");
			}
			const fs::path destination(entry.substr(1));
			const auto     path     = (stage / destination.relative_path()).lexically_normal();
			const auto     relative = path.lexically_relative(stage.lexically_normal());
			if (!destination.is_absolute() || relative.empty() ||
			    (relative == "." && entry.front() == 'F') || *relative.begin() == "..") {
				throw std::runtime_error("Install destination escapes DESTDIR: " + entry);
			}
			kCheckPath(path, entry.front() == 'D');
		}
		if (!manifest.eof()) {
			throw std::runtime_error("Cannot read install destination manifest");
		}
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
