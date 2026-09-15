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

	using howdy::test::Expect;
	using howdy::test::WriteFile;

	auto BoundaryChecks(const std::filesystem::path &fixture) -> bool {
		namespace fs = std::filesystem;
		using howdy::native::CheckSecureRootOwnedDirectoryTree;
		using howdy::native::file_security_internal::ValidationRoot;
		const auto outside = fixture / "world-writable";
		const auto root    = outside / "boundary";
		const auto child   = root / "child";
		const auto nested  = child / "nested";
		fs::create_directories(nested);
		fs::create_directory(outside / "boundary-sibling");
		bool ok =
		    Expect(chmod(outside.c_str(), 01777) == 0, "world-writable sticky external ancestor");
		ok &= Expect(chmod(root.c_str(), 0700) == 0 && chmod(child.c_str(), 0700) == 0 &&
		                 chmod(nested.c_str(), 0700) == 0,
		             "secure controlled boundary tree");
		const ValidationRoot boundary{root};
		const auto check = [&](const fs::path &target) -> howdy::native::SecurePathCheckResult {
			return CheckSecureRootOwnedDirectoryTree(target, "Boundary directory", geteuid(),
			                                         boundary);
		};
		ok &= Expect(check(root).ok, "boundary equal to target is checked and accepted");
		ok &= Expect(check(child).ok, "secure child ignores external writable ancestor");
		ok &= Expect(check(nested).ok, "secure nested child is accepted");
		ok &= Expect(check(root / "." / "child" / "nested" / "").ok,
		             "dot and trailing separator preserve containment");
		ok &= Expect(check(outside).error_message.contains("outside validation boundary"),
		             "target outside boundary is rejected");
		ok &= Expect(check(outside / "boundary-sibling")
		                 .error_message.contains("outside validation boundary"),
		             "same-prefix sibling is rejected");
		ok &= Expect(check(root / ".." / "boundary-sibling")
		                 .error_message.contains("outside validation boundary"),
		             "parent escape is rejected");
		ok &= Expect(
		    check(child / ".." / "child").error_message.contains("outside validation boundary"),
		    "parent component is rejected even when it returns inside");
		ok &= Expect(!check("child").ok, "custom boundary rejects relative target");
		ok &= Expect(!CheckSecureRootOwnedDirectoryTree(child, "Boundary", std::nullopt,
		                                                {root / ".." / "boundary"})
		                  .ok,
		             "boundary containing parent component is rejected");
		ok &= Expect(
		    !CheckSecureRootOwnedDirectoryTree(child, "Boundary", std::nullopt, {"relative"}).ok,
		    "relative boundary is rejected");
		ok &= Expect(!check(fs::path(root.string() + std::string(1, '\0') + "/child")).ok,
		             "embedded NUL is rejected");
		ok &= Expect(chmod(root.c_str(), 0777) == 0, "make boundary insecure");
		ok &= Expect(!check(root).ok && !check(nested).ok,
		             "insecure boundary is rejected including equality");
		ok &= Expect(chmod(root.c_str(), 0700) == 0, "restore boundary");
		ok &= Expect(chmod(child.c_str(), 0775) == 0, "make intermediate insecure");
		ok &= Expect(!check(nested).ok, "insecure intermediate is rejected");
		ok &= Expect(chmod(child.c_str(), 0700) == 0, "restore intermediate");
		fs::create_directory_symlink(outside, root / "escape");
		fs::create_directory_symlink(child, root / "inside-link");
		fs::create_directory_symlink(root, outside / "root-link");
		ok &= Expect(!check(root / "escape" / "boundary-sibling").ok, "symlink escape is rejected");
		ok &= Expect(!check(root / "inside-link" / "nested").ok,
		             "internal directory symlink is rejected");
		for (const auto &suffix : {fs::path{}, fs::path{"."}}) {
			const auto link =
			    suffix.empty() ? outside / "root-link" : outside / "root-link" / suffix;
			ok &= Expect(
			    !CheckSecureRootOwnedDirectoryTree(link, "Boundary", std::nullopt, {link}).ok,
			    "symlink boundary is rejected including trailing dot");
		}
		const auto production = CheckSecureRootOwnedDirectoryTree(nested, "Boundary", std::nullopt);
		const auto full_root =
		    CheckSecureRootOwnedDirectoryTree(nested, "Boundary", std::nullopt, {"/"});
		ok &= Expect(!production.ok && !full_root.ok &&
		                 production.error_message == full_root.error_message,
		             "default wrapper still walks from filesystem root and rejects external "
		             "writable ancestor");
		return ok;
	}

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
		return howdy::native::CheckSecureRootOwnedDirectoryTree(
		    path, "Test directory tree", std::nullopt,
		    {std::filesystem::current_path() / "howdy-file-security-test"});
	}

	auto SecureFileWithDirectory(const std::filesystem::path &path)
	    -> howdy::native::SecurePathCheckResult {
		return howdy::native::CheckSecureRootOwnedFileWithDirectory(
		    path, {.directory = "Test parent directory", .file = "Test file"}, std::nullopt,
		    {std::filesystem::current_path() / "howdy-file-security-test"});
	}

	auto SecureFdWithDirectory(int fd, const std::filesystem::path &path)
	    -> howdy::native::SecurePathCheckResult {
		return howdy::native::CheckSecureRootOwnedFdWithDirectory(
		    fd, path, {.directory = "Test parent directory", .file = "Test file"}, std::nullopt,
		    {std::filesystem::current_path() / "howdy-file-security-test"});
	}

}  // namespace

auto main() -> int {
	namespace fs = std::filesystem;

	bool            ok        = true;
	const auto      temp_root = fs::current_path() / "howdy-file-security-test";
	std::error_code ec;
	fs::remove_all(temp_root, ec);
	fs::create_directories(temp_root, ec);
	ok &= Expect(!ec, "create temp root");
	ok &= Expect(chmod(temp_root.c_str(), 0700) == 0, "secure fixture boundary");
	ok &= BoundaryChecks(temp_root);

	const auto safe_file = temp_root / "safe-file";
	const auto safe_dir  = temp_root / "safe-dir";
	ok &= Expect(WriteFile(safe_file, "safe"), "write safe file");
	fs::create_directory(safe_dir, ec);
	ok &= Expect(!ec, "create safe dir");

	ok &= Expect(SecureFile(safe_file).ok, "safe regular file is accepted");
	ok &= Expect(SecureDir(safe_dir).ok, "safe directory is accepted");

	int safe_fd = open(safe_file.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	ok &= Expect(safe_fd >= 0, "open safe file descriptor");
	if (safe_fd >= 0) {
		ok &= Expect(SecureFd(safe_fd, safe_file).ok, "safe descriptor is accepted");
		ok &= Expect(!SecureFd(safe_fd, safe_file, static_cast<uid_t>(geteuid() == 0 ? 1 : 0)).ok,
		             "descriptor with wrong owner is rejected");
		ok &= Expect(SecureFdWithDirectory(safe_fd, safe_file).ok,
		             "safe descriptor with directory is accepted");
		close(safe_fd);
	}
	errno                 = EACCES;
	const auto invalid_fd = SecureFd(-1, safe_file);
	ok &= Expect(!invalid_fd.ok && invalid_fd.error_code == EBADF,
	             "invalid descriptor is rejected with EBADF");

	ok &= Expect(chmod(safe_file.c_str(), 0664) == 0, "make file group-writable");
	ok &= Expect(!SecureFile(safe_file).ok, "group-writable file is rejected");
	int group_writable_fd = open(safe_file.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	ok &= Expect(group_writable_fd >= 0, "open group-writable file descriptor");
	if (group_writable_fd >= 0) {
		ok &= Expect(!SecureFd(group_writable_fd, safe_file).ok,
		             "group-writable descriptor is rejected");
		close(group_writable_fd);
	}
	ok &= Expect(chmod(safe_file.c_str(), 0644) == 0, "restore file mode");

	ok &= Expect(chmod(safe_file.c_str(), 0666) == 0, "make file world-writable");
	ok &= Expect(!SecureFile(safe_file).ok, "world-writable file is rejected");
	ok &= Expect(chmod(safe_file.c_str(), 0644) == 0, "restore file mode after world writable");

	ok &= Expect(chmod(safe_dir.c_str(), 0775) == 0, "make dir group-writable");
	ok &= Expect(!SecureDir(safe_dir).ok, "group-writable directory is rejected");
	ok &= Expect(chmod(safe_dir.c_str(), 0755) == 0, "restore dir mode");

	ok &= Expect(chmod(safe_dir.c_str(), 0777) == 0, "make dir world-writable");
	ok &= Expect(!SecureDir(safe_dir).ok, "world-writable directory is rejected");
	ok &= Expect(chmod(safe_dir.c_str(), 0755) == 0, "restore dir mode after world writable");

	int dir_fd = open(safe_dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	ok &= Expect(dir_fd >= 0, "open directory descriptor");
	if (dir_fd >= 0) {
		ok &= Expect(howdy::native::CheckSecureFd(dir_fd, howdy::native::SecurePathKind::kDirectory,
		                                          safe_dir, "Test directory", std::nullopt)
		                 .ok,
		             "directory descriptor is accepted for directory check");
		ok &= Expect(!SecureFd(dir_fd, safe_dir).ok,
		             "directory descriptor is rejected for regular file check");
		close(dir_fd);
	}

	const auto symlink_path = temp_root / "file-symlink";
	fs::remove(symlink_path, ec);
	ec.clear();
	if (symlink(safe_file.c_str(), symlink_path.c_str()) == 0) {
		ok &= Expect(!SecureFile(symlink_path).ok, "symlink is rejected");
	} else {
		std::cerr << "SKIP: symlink creation failed: " << std::strerror(errno) << "\n";
	}

	const auto hardlink_path = temp_root / "file-hardlink";
	fs::remove(hardlink_path, ec);
	ec.clear();
	ok &= Expect(link(safe_file.c_str(), hardlink_path.c_str()) == 0, "create hardlink");
	ok &=
	    Expect(!SecureFile(safe_file).ok, "hardlinked regular file is rejected by current policy");
	int hardlink_fd = open(safe_file.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	ok &= Expect(hardlink_fd >= 0, "open hardlinked file descriptor");
	if (hardlink_fd >= 0) {
		ok &= Expect(!SecureFd(hardlink_fd, safe_file).ok,
		             "hardlinked descriptor is rejected by current policy");
		close(hardlink_fd);
	}
	fs::remove(hardlink_path, ec);
	ec.clear();
	ok &= Expect(SecureFile(safe_file).ok, "file is accepted after hardlink removal");

	const auto missing_file = temp_root / "missing-file";
	ok &= Expect(!SecureFile(missing_file).ok, "missing file is rejected");

	const auto missing_parent_file = temp_root / "missing-parent" / "config.ini";
	ok &= Expect(!SecureFileWithDirectory(missing_parent_file).ok,
	             "missing parent directory is rejected before file use");

	const auto insecure_parent = temp_root / "insecure-parent";
	const auto child_dir       = insecure_parent / "child";
	fs::create_directories(child_dir, ec);
	ok &= Expect(!ec, "create insecure parent tree");
	ok &= Expect(chmod(insecure_parent.c_str(), 0777) == 0, "make ancestor world-writable");
	ok &= Expect(!SecureDirTree(child_dir).ok, "insecure ancestor directory is rejected");
	const auto child_file = child_dir / "config.ini";
	ok &= Expect(WriteFile(child_file, "content"), "write file under insecure dir");
	ok &= Expect(!SecureFileWithDirectory(child_file).ok,
	             "file under insecure directory is rejected");
	ok &= Expect(chmod(insecure_parent.c_str(), 0755) == 0, "restore ancestor mode");

	const auto real_dir     = temp_root / "real-dir";
	const auto real_file    = real_dir / "model.dat";
	const auto symlink_dir  = temp_root / "symlink-dir";
	const auto linked_child = symlink_dir / "model.dat";
	fs::create_directories(real_dir, ec);
	ok &= Expect(!ec, "create real symlink target directory");
	ok &= Expect(WriteFile(real_file, "content"), "write symlink target child file");
	fs::remove(symlink_dir, ec);
	ec.clear();
	if (symlink(real_dir.c_str(), symlink_dir.c_str()) == 0) {
		ok &= Expect(!SecureFileWithDirectory(linked_child).ok,
		             "directory traversal through symlink is rejected");
	} else {
		std::cerr << "SKIP: directory symlink creation failed: " << std::strerror(errno) << "\n";
	}

	const auto closed_parent = temp_root / "closed-parent";
	const auto closed_file   = closed_parent / "closed-file";
	fs::create_directory(closed_parent, ec);
	ok &= Expect(!ec, "create closed parent");
	ok &= Expect(WriteFile(closed_file, "content"), "write file under closed parent");
	if (geteuid() != 0) {
		ok &= Expect(chmod(closed_parent.c_str(), 0000) == 0, "remove parent search permission");
		const auto closed_result = SecureFileWithDirectory(closed_file);
		ok &= Expect(!closed_result.ok, "checks fail closed on lstat failure");
		ok &= Expect(chmod(closed_parent.c_str(), 0755) == 0, "restore closed parent mode");
	} else {
		std::cerr << "SKIP: lstat failure permission check while running as root\n";
	}

	fs::remove_all(temp_root, ec);

	return ok ? 0 : 1;
}
