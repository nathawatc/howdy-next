#include "config/runtime_config_loader.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <sys/stat.h>

namespace {

	auto expect(bool condition, const std::string &message) -> bool {
		if (!condition) {
			std::cerr << "FAIL: " << message << "\n";
			return false;
		}
		return true;
	}

	auto write_file(const std::filesystem::path &path, const std::string &content) -> bool {
		std::ofstream out(path);
		if (!out.is_open()) {
			return false;
		}
		out << content;
		return out.good();
	}

	auto expect_failure(const howdy::native::RuntimeConfigLoadResult &result,
	                    howdy::native::RuntimeConfigLoadStatus status, const std::string &message)
	    -> bool {
		bool ok = true;
		ok &= expect(result.status == status, message + " status");
		ok &= expect(result.config == nullptr, message + " has no partial config");
		ok &= expect(!result.error_message.empty(), message + " has stable caller error");
		return ok;
	}

	struct TemporaryDirectory {
		std::filesystem::path path;

		~TemporaryDirectory() {
			std::error_code ec;
			std::filesystem::remove_all(path, ec);
		}
	};

	auto create_temp_directory() -> std::optional<std::filesystem::path> {
		const auto template_path =
		    std::filesystem::temp_directory_path() / "howdy-runtime-config-loader-test-XXXXXX";
		const auto        template_string = template_path.string();
		std::vector<char> path_buffer(template_string.begin(), template_string.end());
		path_buffer.push_back('\0');

		char *created = mkdtemp(path_buffer.data());
		if (created == nullptr) {
			return std::nullopt;
		}
		return std::filesystem::path(created);
	}

}  // namespace

auto main() -> int {
	namespace fs = std::filesystem;
	using howdy::native::RuntimeConfigLoadStatus;

	bool ok = true;

	const auto temp_directory = create_temp_directory();
	if (!temp_directory.has_value()) {
		std::cerr << "FAIL: create unique temp root\n";
		return 1;
	}
	const TemporaryDirectory temp_directory_guard{.path = *temp_directory};
	const auto              &temp_root = temp_directory_guard.path;
	ok &= expect(chmod(temp_root.c_str(), 0755) == 0, "secure temp root");

	const auto valid_path = temp_root / "valid.ini";
	ok &= expect(write_file(valid_path, "[core]\ndisabled = false\n"), "write valid config");
	ok &= expect(chmod(valid_path.c_str(), 0644) == 0, "secure valid config");
	setenv("HOWDY_CONFIG", valid_path.c_str(), 1);
	auto valid = howdy::native::load_runtime_config();
	unsetenv("HOWDY_CONFIG");
	ok &= expect(valid.status == RuntimeConfigLoadStatus::kOk, "valid config loads");
	ok &= expect(valid.path == valid_path, "default loader resolves config path");
	ok &= expect(valid.config != nullptr, "valid config returns reader");
	ok &= expect(valid.error_message.empty(), "valid config has no error");

	const auto missing_path = temp_root / "missing.ini";
	const auto missing      = howdy::native::load_runtime_config(missing_path, std::nullopt);
	ok &= expect_failure(missing, RuntimeConfigLoadStatus::kPathError, "missing config");

	const auto malformed_path = temp_root / "malformed.ini";
	ok &= expect(write_file(malformed_path, "[core\n"), "write malformed config");
	const auto malformed = howdy::native::load_runtime_config(malformed_path, std::nullopt);
	ok &= expect_failure(malformed, RuntimeConfigLoadStatus::kParseError, "malformed config");
	ok &= expect(malformed.error_message.contains("Failed to parse config: "),
	             "parse error message includes parse prefix");
	ok &= expect(malformed.error_message.contains(malformed_path.string()),
	             "parse error message includes path");
	ok &= expect(malformed.error_message.contains("(error "),
	             "parse error message includes parser code");

	const auto invalid_path = temp_root / "invalid.ini";
	ok &= expect(write_file(invalid_path, "[video]\ntimeout = 0\n"), "write invalid config");
	const auto invalid = howdy::native::load_runtime_config(invalid_path, std::nullopt);
	ok &= expect_failure(invalid, RuntimeConfigLoadStatus::kInvalidRuntimeValue,
	                     "invalid runtime config");
	ok &= expect(invalid.error_message.contains("Invalid runtime config in "),
	             "invalid runtime message includes prefix");
	ok &= expect(invalid.error_message.contains(invalid_path.string()),
	             "invalid runtime message includes path");
	ok &= expect(invalid.error_message.contains(invalid_path.string() + ": "),
	             "invalid runtime message separates path and validation detail");

	const auto insecure_path = temp_root / "insecure.ini";
	ok &= expect(write_file(insecure_path, "[core]\ndisabled = false\n"), "write insecure config");
	ok &= expect(chmod(insecure_path.c_str(), 0666) == 0, "make config insecure");
	const auto insecure = howdy::native::load_runtime_config(insecure_path, std::nullopt);
	ok &= expect_failure(insecure, RuntimeConfigLoadStatus::kPathError, "insecure config");

	const auto      insecure_root = temp_root / "insecure-root";
	std::error_code ec;
	fs::create_directories(insecure_root, ec);
	ok &= expect(!ec, "create insecure trust root");
	const auto insecure_root_config = insecure_root / "config.ini";
	ok &= expect(write_file(insecure_root_config, "[core]\ndisabled = false\n"),
	             "write insecure trust-root config");
	ok &= expect(chmod(insecure_root.c_str(), 0777) == 0, "make trust root insecure");
	const auto insecure_trust =
	    howdy::native::load_runtime_config(insecure_root_config, std::nullopt);
	ok &=
	    expect_failure(insecure_trust, RuntimeConfigLoadStatus::kPathError, "insecure trust root");

	if (!ok) {
		return 1;
	}
	return 0;
}
