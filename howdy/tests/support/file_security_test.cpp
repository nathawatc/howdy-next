#include "support/file_security.hpp"
#include "test_support.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <optional>
#include <unistd.h>

#include <sys/stat.h>

namespace {

	using howdy::test::expect;
	using howdy::test::write_file;

	auto SecureFile(const std::filesystem::path &path) -> howdy::native::SecurePathCheckResult {
		return howdy::native::CheckSecureRootOwnedFile(path, "Test file", std::nullopt);
	}

	auto SecureFd(int fd, const std::filesystem::path &path,
	              std::optional<uid_t> owner_uid = std::nullopt)
	    -> howdy::native::SecurePathCheckResult {
		return howdy::native::CheckSecureRootOwnedFd(fd, path, "Test file", owner_uid);
	}

	auto SecureDir(const std::filesystem::path &path) -> howdy::native::SecurePathCheckResult {
		return howdy::native::CheckSecureRootOwnedDirectory(path, "Test directory", std::nullopt);
	}

	auto SecureDirTree(const std::filesystem::path &path) -> howdy::native::SecurePathCheckResult {
		return howdy::native::CheckSecureRootOwnedDirectoryTree(path, "Test directory tree",
		                                                        std::nullopt);
	}

	auto SecureFileWithDirectory(const std::filesystem::path &path)
	    -> howdy::native::SecurePathCheckResult {
		return howdy::native::CheckSecureRootOwnedFileWithDirectory(
		    path, {.directory = "Test parent directory", .file = "Test file"}, std::nullopt);
	}

	auto SecureFdWithDirectory(int fd, const std::filesystem::path &path)
	    -> howdy::native::SecurePathCheckResult {
		return howdy::native::CheckSecureRootOwnedFdWithDirectory(
		    fd, path, {.directory = "Test parent directory", .file = "Test file"}, std::nullopt);
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

	ok &= expect(SecureFile(safe_file).ok, "safe regular file is accepted");
	ok &= expect(SecureDir(safe_dir).ok, "safe directory is accepted");

	int safe_fd = open(safe_file.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	ok &= expect(safe_fd >= 0, "open safe file descriptor");
	if (safe_fd >= 0) {
		ok &= expect(SecureFd(safe_fd, safe_file).ok, "safe descriptor is accepted");
		ok &= expect(!SecureFd(safe_fd, safe_file, static_cast<uid_t>(geteuid() == 0 ? 1 : 0)).ok,
		             "descriptor with wrong owner is rejected");
		ok &= expect(SecureFdWithDirectory(safe_fd, safe_file).ok,
		             "safe descriptor with directory is accepted");
		close(safe_fd);
	}
	errno                 = EACCES;
	const auto invalid_fd = SecureFd(-1, safe_file);
	ok &= expect(!invalid_fd.ok && invalid_fd.error_code == EBADF,
	             "invalid descriptor is rejected with EBADF");

	ok &= expect(chmod(safe_file.c_str(), 0664) == 0, "make file group-writable");
	ok &= expect(!SecureFile(safe_file).ok, "group-writable file is rejected");
	int group_writable_fd = open(safe_file.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	ok &= expect(group_writable_fd >= 0, "open group-writable file descriptor");
	if (group_writable_fd >= 0) {
		ok &= expect(!SecureFd(group_writable_fd, safe_file).ok,
		             "group-writable descriptor is rejected");
		close(group_writable_fd);
	}
	ok &= expect(chmod(safe_file.c_str(), 0644) == 0, "restore file mode");

	ok &= expect(chmod(safe_file.c_str(), 0666) == 0, "make file world-writable");
	ok &= expect(!SecureFile(safe_file).ok, "world-writable file is rejected");
	ok &= expect(chmod(safe_file.c_str(), 0644) == 0, "restore file mode after world writable");

	ok &= expect(chmod(safe_dir.c_str(), 0775) == 0, "make dir group-writable");
	ok &= expect(!SecureDir(safe_dir).ok, "group-writable directory is rejected");
	ok &= expect(chmod(safe_dir.c_str(), 0755) == 0, "restore dir mode");

	ok &= expect(chmod(safe_dir.c_str(), 0777) == 0, "make dir world-writable");
	ok &= expect(!SecureDir(safe_dir).ok, "world-writable directory is rejected");
	ok &= expect(chmod(safe_dir.c_str(), 0755) == 0, "restore dir mode after world writable");

	int dir_fd = open(safe_dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	ok &= expect(dir_fd >= 0, "open directory descriptor");
	if (dir_fd >= 0) {
		ok &= expect(howdy::native::CheckSecureFd(dir_fd, howdy::native::SecurePathKind::kDirectory,
		                                          safe_dir, "Test directory", std::nullopt)
		                 .ok,
		             "directory descriptor is accepted for directory check");
		ok &= expect(!SecureFd(dir_fd, safe_dir).ok,
		             "directory descriptor is rejected for regular file check");
		close(dir_fd);
	}

	const auto symlink_path = temp_root / "file-symlink";
	fs::remove(symlink_path, ec);
	ec.clear();
	if (symlink(safe_file.c_str(), symlink_path.c_str()) == 0) {
		ok &= expect(!SecureFile(symlink_path).ok, "symlink is rejected");
	} else {
		std::cerr << "SKIP: symlink creation failed: " << std::strerror(errno) << "\n";
	}

	const auto hardlink_path = temp_root / "file-hardlink";
	fs::remove(hardlink_path, ec);
	ec.clear();
	ok &= expect(link(safe_file.c_str(), hardlink_path.c_str()) == 0, "create hardlink");
	ok &=
	    expect(!SecureFile(safe_file).ok, "hardlinked regular file is rejected by current policy");
	int hardlink_fd = open(safe_file.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	ok &= expect(hardlink_fd >= 0, "open hardlinked file descriptor");
	if (hardlink_fd >= 0) {
		ok &= expect(!SecureFd(hardlink_fd, safe_file).ok,
		             "hardlinked descriptor is rejected by current policy");
		close(hardlink_fd);
	}
	fs::remove(hardlink_path, ec);
	ec.clear();
	ok &= expect(SecureFile(safe_file).ok, "file is accepted after hardlink removal");

	const auto missing_file = temp_root / "missing-file";
	ok &= expect(!SecureFile(missing_file).ok, "missing file is rejected");

	const auto missing_parent_file = temp_root / "missing-parent" / "config.ini";
	ok &= expect(!SecureFileWithDirectory(missing_parent_file).ok,
	             "missing parent directory is rejected before file use");

	const auto insecure_parent = temp_root / "insecure-parent";
	const auto child_dir       = insecure_parent / "child";
	fs::create_directories(child_dir, ec);
	ok &= expect(!ec, "create insecure parent tree");
	ok &= expect(chmod(insecure_parent.c_str(), 0777) == 0, "make ancestor world-writable");
	ok &= expect(!SecureDirTree(child_dir).ok, "insecure ancestor directory is rejected");
	const auto child_file = child_dir / "config.ini";
	ok &= expect(write_file(child_file, "content"), "write file under insecure dir");
	ok &= expect(!SecureFileWithDirectory(child_file).ok,
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
		ok &= expect(!SecureFileWithDirectory(linked_child).ok,
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
		const auto closed_result = SecureFileWithDirectory(closed_file);
		ok &= expect(!closed_result.ok, "checks fail closed on lstat failure");
		ok &= expect(chmod(closed_parent.c_str(), 0755) == 0, "restore closed parent mode");
	} else {
		std::cerr << "SKIP: lstat failure permission check while running as root\n";
	}

	fs::remove_all(temp_root, ec);

	return ok ? 0 : 1;
}
