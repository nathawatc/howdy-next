#include "test_support.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <stdlib.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

	using Cleanup = howdy::test::TemporaryDirectory;

	auto IsAbsent(const fs::path &path) -> bool {
		std::error_code error;
		const auto      status = fs::symlink_status(path, error);
		return status.type() == fs::file_type::not_found &&
		       (!error || error == std::errc::no_such_file_or_directory);
	}

	auto IsPrefixName(std::string_view name) -> bool {
		constexpr std::string_view stem = "howdy-installed-pam-e2e-";
		if (!name.starts_with(stem) || name.size() == stem.size()) {
			return false;
		}
		return std::ranges::all_of(name.substr(stem.size()), [](char character) -> bool {
			return (character >= 'A' && character <= 'Z') ||
			       (character >= 'a' && character <= 'z') ||
			       (character >= '0' && character <= '9') || character == '.' || character == '_' ||
			       character == '-';
		});
	}

	auto IsCanonicalSpelling(const fs::path &path) -> bool {
		std::error_code error;
		const auto      canonical = fs::weakly_canonical(path, error);
		return !error && path.is_absolute() && path.string() == canonical.string();
	}

	// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
	auto ValidatePrefix(const fs::path &input, const fs::path &expected_parent) -> bool {
		if (!IsCanonicalSpelling(input) || !IsPrefixName(input.filename().string()) ||
		    !IsAbsent(input)) {
			return false;
		}
		std::error_code error;
		const auto      parent = fs::canonical(input.parent_path(), error);
		return !error && parent == expected_parent;
	}

	auto Under(const fs::path &prefix, const fs::path &input) -> bool {
		return input.string().starts_with(prefix.string() + "/");
	}

	auto ValidateSubdir(const fs::path &prefix, const fs::path &input) -> bool {
		return IsCanonicalSpelling(input) && Under(prefix, input);
	}

	auto ValidateInstalled(const fs::path &prefix, std::string_view type, const fs::path &input)
	    -> std::optional<fs::path> {
		if (!Under(prefix, input) || fs::is_symlink(fs::symlink_status(input))) {
			return std::nullopt;
		}
		if ((type == "file" && !fs::is_regular_file(input)) ||
		    (type == "directory" && !fs::is_directory(input)) ||
		    (type != "file" && type != "directory")) {
			return std::nullopt;
		}
		std::error_code error;
		auto            canonical = fs::canonical(input, error);
		if (error || !Under(prefix, canonical)) {
			return std::nullopt;
		}
		return canonical;
	}

	auto Require(bool condition, const std::string &message) -> void {
		if (!condition) {
			throw std::runtime_error(message);
		}
	}

	auto SelfTest() -> void {
		std::string template_path = "/tmp/howdy-e2e-path-validation.XXXXXX";
		const char *created       = mkdtemp(template_path.data());
		Require(created != nullptr, "cannot create path-validation fixture");
		const fs::path parent = fs::canonical(created);
		const Cleanup  cleanup(parent);
		const auto     other = parent / "other";
		fs::create_directory(other);
		const auto valid =
		    parent / ("howdy-installed-pam-e2e-validation-" + std::to_string(getpid()));
		const auto marker =
		    fs::path("/tmp") / ("howdy-e2e-path-validation-" + std::to_string(getpid()));
		Require(IsAbsent(marker), "path-validation marker already exists");
		Require(ValidatePrefix(valid, parent), "valid isolated prefix rejected");
		for (const auto &rejected : {
		         fs::path(valid.string() + "/../../etc"),
		         valid / "subdir",
		         other / "howdy-installed-pam-e2e-x",
		         parent / "howdy-installed-pam-e2e-",
		         fs::path(parent.string() + "//howdy-installed-pam-e2e-x"),
		         fs::path((parent / "howdy-installed-pam-e2e-x").string() + "/."),
		     }) {
			Require(!ValidatePrefix(rejected, parent),
			        "unsafe prefix accepted: " + rejected.string());
		}
		Require(ValidatePrefix(valid, parent), "valid prefix rejected after invalid cases");
		for (const auto &rejected :
		     {fs::path("/etc/howdy"), fs::path("/usr/libexec/howdy"), valid / "../etc", valid}) {
			Require(!ValidateSubdir(valid, rejected),
			        "unsafe install path accepted: " + rejected.string());
		}
		for (const auto &accepted : {
		         valid / "lib/security",
		         valid / "libexec/howdy",
		         valid / "etc/howdy",
		         valid / "etc/howdy/models",
		     }) {
			Require(ValidateSubdir(valid, accepted),
			        "valid install path rejected: " + accepted.string());
		}
		Require(IsAbsent(marker) && IsAbsent(valid), "path validation mutated filesystem");

		const auto installed_dir = valid / "lib/security";
		fs::create_directories(installed_dir);
		const auto installed_file = installed_dir / "pam_howdy.so";
		std::ofstream(installed_file) << "fixture";
		Require(ValidateInstalled(valid, "file", installed_file) == installed_file,
		        "installed file rejected");
		Require(ValidateInstalled(valid, "directory", installed_dir) == installed_dir,
		        "installed directory rejected");
		fs::create_symlink(installed_file, installed_dir / "linked.so");
		Require(!ValidateInstalled(valid, "file", installed_dir / "linked.so"),
		        "installed symlink accepted");
		Require(!ValidateInstalled(valid, "file", parent / "outside.so"),
		        "path outside prefix accepted");
		Require(!ValidateInstalled(valid, "file", installed_dir / "missing.so"),
		        "missing installed file accepted");
		std::cout << "Path validation: passed without filesystem mutation\n";
	}

}  // namespace

auto main(int argc, char **argv) -> int {
	std::vector<std::string> args;
	for (const auto *argument : std::span(argv, static_cast<std::size_t>(argc))) {
		args.emplace_back(argument);
	}
	try {
		if (args.size() == 2 && args.at(1) == "--self-test") {
			SelfTest();
			return 0;
		}
		if ((args.size() == 3 || args.size() == 4) && args.at(1) == "--validate-prefix") {
			const fs::path parent = args.size() == 4 ? args.at(3) : "/opt";
			if (ValidatePrefix(args.at(2), parent)) {
				std::cout << args.at(2) << '\n';
				return 0;
			}
		} else if (args.size() == 4 && args.at(1) == "--validate-subdir") {
			if (ValidateSubdir(args.at(2), args.at(3))) {
				std::cout << args.at(3) << '\n';
				return 0;
			}
		} else if (args.size() == 5 && args.at(1) == "--validate-installed") {
			const auto canonical = ValidateInstalled(args.at(2), args.at(3), args.at(4));
			if (canonical) {
				std::cout << canonical->string() << '\n';
				return 0;
			}
		}
		std::cerr << "unsafe or invalid path\n";
		return 1;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
