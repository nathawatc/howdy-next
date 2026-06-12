#include "cli/disable_cli.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include <sys/stat.h>

namespace {

	auto write_file(const std::filesystem::path &path, const std::string &content) -> bool {
		std::ofstream out(path);
		if (!out.is_open()) {
			return false;
		}
		out << content;
		return out.good();
	}

	auto read_file(const std::filesystem::path &path) -> std::string {
		std::ifstream in(path);
		if (!in.is_open()) {
			return {};
		}
		return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
	}

	auto expect(bool condition, const std::string &message) -> bool {
		if (!condition) {
			std::cerr << "FAIL: " << message << "\n";
			return false;
		}
		return true;
	}

	auto run_disable(const std::string &argument) -> int {
		std::string           arg = argument;
		std::array<char *, 3> argv{
		    const_cast<char *>("howdy-disable"),
		    arg.data(),
		    nullptr,
		};
		return disable_main(2, argv.data());
	}

}  // namespace

auto main() -> int {
	namespace fs = std::filesystem;

	bool            ok          = true;
	const auto      temp_root   = fs::current_path() / "howdy-disable-cli-test";
	const auto      config_path = temp_root / "config.ini";
	std::error_code ec;

	fs::remove_all(temp_root, ec);
	fs::create_directories(temp_root, ec);
	ok &= expect(!ec, "create temp root");
	ok &= expect(chmod(temp_root.c_str(), 0755) == 0, "secure temp root");
	setenv("HOWDY_CONFIG", config_path.c_str(), 1);

	ok &= expect(write_file(config_path, "[core]\ndisabled = true\n"), "write disabled config");
	ok &= expect(chmod(config_path.c_str(), 0644) == 0, "secure disabled config");
	ok &= expect(run_disable("1") == 1, "disable 1 aborts when already disabled");
	ok &= expect(read_file(config_path).contains("disabled = true\n"),
	             "already disabled config unchanged");

	ok &= expect(write_file(config_path, "[core]\ndisabled = false\n"), "write enabled config");
	ok &= expect(chmod(config_path.c_str(), 0644) == 0, "secure enabled config");
	ok &= expect(run_disable("0") == 1, "disable 0 aborts when already enabled");
	ok &= expect(run_disable("true") == 0, "disable true changes enabled config");
	ok &=
	    expect(read_file(config_path).contains("disabled = true\n"), "config changed to disabled");
	ok &= expect(run_disable("false") == 0, "disable false changes disabled config");
	ok &=
	    expect(read_file(config_path).contains("disabled = false\n"), "config changed to enabled");

	const auto insecure_dir  = temp_root / "insecure-config-dir";
	const auto insecure_path = insecure_dir / "config.ini";
	ok &= expect(fs::create_directories(insecure_dir, ec) || !ec, "create insecure config dir");
	ok &= expect(!ec, "no error creating insecure config dir");
	ok &= expect(write_file(insecure_path, "[core]\ndisabled = false\n"), "write insecure config");
	ok &= expect(chmod(insecure_dir.c_str(), 0777) == 0, "make config dir insecure");
	setenv("HOWDY_CONFIG", insecure_path.c_str(), 1);
	ok &= expect(run_disable("true") == 1, "insecure config path aborts");
	ok &= expect(read_file(insecure_path).contains("disabled = false\n"),
	             "insecure config unchanged");
	ok &= expect(chmod(insecure_dir.c_str(), 0755) == 0, "restore insecure config dir mode");
	setenv("HOWDY_CONFIG", config_path.c_str(), 1);

	ok &= expect(run_disable("invalid") == 1, "invalid argument aborts");
	ok &= expect(write_file(config_path, "[video]\ntimeout = 0\n"), "write invalid runtime config");
	ok &= expect(chmod(config_path.c_str(), 0644) == 0, "secure invalid runtime config");
	ok &= expect(run_disable("false") == 1, "runtime config load failure aborts");

	unsetenv("HOWDY_CONFIG");
	fs::remove_all(temp_root, ec);
	return ok ? 0 : 1;
}
