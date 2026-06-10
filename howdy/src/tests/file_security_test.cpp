#include "common/file_security.hpp"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <unistd.h>

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

	auto expect(bool condition, const std::string &message) -> bool {
		if (!condition) {
			std::cerr << "FAIL: " << message << "\n";
			return false;
		}
		return true;
	}

	auto secure_file(const std::filesystem::path &path) -> howdy::native::SecurePathCheckResult {
		return howdy::native::check_secure_root_owned_file(path, "Test file", std::nullopt);
	}

	auto secure_dir(const std::filesystem::path &path) -> howdy::native::SecurePathCheckResult {
		return howdy::native::check_secure_root_owned_directory(path, "Test directory",
		                                                        std::nullopt);
	}

	auto secure_dir_tree(const std::filesystem::path &path)
	    -> howdy::native::SecurePathCheckResult {
		return howdy::native::check_secure_root_owned_directory_tree(path, "Test directory tree",
		                                                             std::nullopt);
	}

	auto secure_file_with_directory(const std::filesystem::path &path)
	    -> howdy::native::SecurePathCheckResult {
		return howdy::native::check_secure_root_owned_file_with_directory(
		    path, "Test parent directory", "Test file", std::nullopt);
	}

}  // namespace

auto main() -> int {
	namespace fs = std::filesystem;

	bool            ok        = true;
	const auto      temp_root = fs::current_path() / "howdy-file-security-test";
	std::error_code ec;
	fs::remove_all(temp_root, ec);
	fs::create_directories(temp_root, ec);
	ok &= expect(!ec, "create temp root");

	const auto safe_file = temp_root / "safe-file";
	const auto safe_dir  = temp_root / "safe-dir";
	ok &= expect(write_file(safe_file, "safe"), "write safe file");
	fs::create_directory(safe_dir, ec);
	ok &= expect(!ec, "create safe dir");

	ok &= expect(secure_file(safe_file).ok, "safe regular file is accepted");
	ok &= expect(secure_dir(safe_dir).ok, "safe directory is accepted");

	ok &= expect(chmod(safe_file.c_str(), 0664) == 0, "make file group-writable");
	ok &= expect(!secure_file(safe_file).ok, "group-writable file is rejected");
	ok &= expect(chmod(safe_file.c_str(), 0644) == 0, "restore file mode");

	ok &= expect(chmod(safe_file.c_str(), 0666) == 0, "make file world-writable");
	ok &= expect(!secure_file(safe_file).ok, "world-writable file is rejected");
	ok &= expect(chmod(safe_file.c_str(), 0644) == 0, "restore file mode after world writable");

	ok &= expect(chmod(safe_dir.c_str(), 0775) == 0, "make dir group-writable");
	ok &= expect(!secure_dir(safe_dir).ok, "group-writable directory is rejected");
	ok &= expect(chmod(safe_dir.c_str(), 0755) == 0, "restore dir mode");

	ok &= expect(chmod(safe_dir.c_str(), 0777) == 0, "make dir world-writable");
	ok &= expect(!secure_dir(safe_dir).ok, "world-writable directory is rejected");
	ok &= expect(chmod(safe_dir.c_str(), 0755) == 0, "restore dir mode after world writable");

	const auto symlink_path = temp_root / "file-symlink";
	fs::remove(symlink_path, ec);
	ec.clear();
	if (symlink(safe_file.c_str(), symlink_path.c_str()) == 0) {
		ok &= expect(!secure_file(symlink_path).ok, "symlink is rejected");
	} else {
		std::cerr << "SKIP: symlink creation failed: " << std::strerror(errno) << "\n";
	}

	const auto hardlink_path = temp_root / "file-hardlink";
	fs::remove(hardlink_path, ec);
	ec.clear();
	ok &= expect(link(safe_file.c_str(), hardlink_path.c_str()) == 0, "create hardlink");
	ok &=
	    expect(!secure_file(safe_file).ok, "hardlinked regular file is rejected by current policy");
	fs::remove(hardlink_path, ec);
	ec.clear();
	ok &= expect(secure_file(safe_file).ok, "file is accepted after hardlink removal");

	const auto missing_file = temp_root / "missing-file";
	ok &= expect(!secure_file(missing_file).ok, "missing file is rejected");

	const auto missing_parent_file = temp_root / "missing-parent" / "config.ini";
	ok &= expect(!secure_file_with_directory(missing_parent_file).ok,
	             "missing parent directory is rejected before file use");

	const auto insecure_parent = temp_root / "insecure-parent";
	const auto child_dir       = insecure_parent / "child";
	fs::create_directories(child_dir, ec);
	ok &= expect(!ec, "create insecure parent tree");
	ok &= expect(chmod(insecure_parent.c_str(), 0777) == 0, "make ancestor world-writable");
	ok &= expect(!secure_dir_tree(child_dir).ok, "insecure ancestor directory is rejected");
	const auto child_file = child_dir / "config.ini";
	ok &= expect(write_file(child_file, "content"), "write file under insecure dir");
	ok &= expect(!secure_file_with_directory(child_file).ok,
	             "file under insecure directory is rejected");
	ok &= expect(chmod(insecure_parent.c_str(), 0755) == 0, "restore ancestor mode");

	const auto real_dir     = temp_root / "real-dir";
	const auto real_file    = real_dir / "model.dat";
	const auto symlink_dir  = temp_root / "symlink-dir";
	const auto linked_child = symlink_dir / "model.dat";
	fs::create_directories(real_dir, ec);
	ok &= expect(!ec, "create real symlink target directory");
	ok &= expect(write_file(real_file, "content"), "write symlink target child file");
	fs::remove(symlink_dir, ec);
	ec.clear();
	if (symlink(real_dir.c_str(), symlink_dir.c_str()) == 0) {
		ok &= expect(!secure_file_with_directory(linked_child).ok,
		             "directory traversal through symlink is rejected");
	} else {
		std::cerr << "SKIP: directory symlink creation failed: " << std::strerror(errno) << "\n";
	}

	const auto closed_parent = temp_root / "closed-parent";
	const auto closed_file   = closed_parent / "closed-file";
	fs::create_directory(closed_parent, ec);
	ok &= expect(!ec, "create closed parent");
	ok &= expect(write_file(closed_file, "content"), "write file under closed parent");
	if (geteuid() != 0) {
		ok &= expect(chmod(closed_parent.c_str(), 0000) == 0, "remove parent search permission");
		const auto closed_result = secure_file_with_directory(closed_file);
		ok &= expect(!closed_result.ok, "checks fail closed on lstat failure");
		ok &= expect(chmod(closed_parent.c_str(), 0755) == 0, "restore closed parent mode");
	} else {
		std::cerr << "SKIP: lstat failure permission check while running as root\n";
	}

	fs::remove_all(temp_root, ec);

	return ok ? 0 : 1;
}
