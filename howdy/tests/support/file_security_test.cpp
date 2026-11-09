#include "support/bit_flags.hpp"
#include "support/file_security.hpp"
#include "support/file_security/validation_root.hpp"
#include "test_support.hpp"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>

namespace {

	using howdy::native::CreateSecureDirectoryTree;
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

	auto SecureFile(const std::filesystem::path &path,
	                std::optional<uid_t>         owner_uid = std::nullopt)
	    -> howdy::native::SecurePathCheckResult {
		return howdy::native::CheckSecureRootOwnedFile(path, "Test file", owner_uid);
	}

	auto SecureFd(int fd, const std::filesystem::path &path,
	              std::optional<uid_t> owner_uid = std::nullopt)
	    -> howdy::native::SecurePathCheckResult {
		return howdy::native::CheckSecureRootOwnedFd(fd, path, "Test file", owner_uid);
	}

	auto SecureDir(const std::filesystem::path &path, std::optional<uid_t> owner_uid = std::nullopt)
	    -> howdy::native::SecurePathCheckResult {
		return howdy::native::CheckSecureRootOwnedDirectory(path, "Test directory", owner_uid);
	}

	auto SecureDirTree(const std::filesystem::path &path,
	                   std::optional<uid_t>         owner_uid = std::nullopt)
	    -> howdy::native::SecurePathCheckResult {
		return howdy::native::CheckSecureRootOwnedDirectoryTree(
		    path, "Test directory tree", owner_uid,
		    {std::filesystem::current_path() / "howdy-file-security-test"});
	}

	auto SecureFileWithDirectory(const std::filesystem::path &path,
	                             std::optional<uid_t>         owner_uid = std::nullopt)
	    -> howdy::native::SecurePathCheckResult {
		return howdy::native::CheckSecureRootOwnedFileWithDirectory(
		    path, {.directory = "Test parent directory", .file = "Test file"}, owner_uid,
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

	const auto        wrong_owner = static_cast<uid_t>(geteuid() == 0 ? 1 : 0);
	const char *const owner_text  = "must be owned by UID";
	ok &= Expect(SecureFile(safe_file, geteuid()).ok && SecureDir(safe_dir, geteuid()).ok,
	             "matching owner is accepted by path checks");
	ok &= Expect(SecureFile(safe_file, wrong_owner).error_message.contains(owner_text),
	             "file with wrong owner is rejected");
	ok &= Expect(SecureDir(safe_dir, wrong_owner).error_message.contains(owner_text),
	             "directory with wrong owner is rejected");
	ok &= Expect(SecureDirTree(safe_dir, wrong_owner).error_message.contains(owner_text),
	             "directory tree with wrong owner is rejected");
	ok &= Expect(SecureFileWithDirectory(safe_file, wrong_owner).error_message.contains(owner_text),
	             "file with directory and wrong owner is rejected");

	int const safe_fd = open(safe_file.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
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
	int const group_writable_fd = open(safe_file.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
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

	int const dir_fd = open(safe_dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
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
	int const hardlink_fd = open(safe_file.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
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

	const auto missing_parent_file = temp_root / "missing-parent" / "config.toml";
	ok &= Expect(!SecureFileWithDirectory(missing_parent_file).ok,
	             "missing parent directory is rejected before file use");

	const auto insecure_parent = temp_root / "insecure-parent";
	const auto child_dir       = insecure_parent / "child";
	fs::create_directories(child_dir, ec);
	ok &= Expect(!ec, "create insecure parent tree");
	ok &= Expect(chmod(insecure_parent.c_str(), 0777) == 0, "make ancestor world-writable");
	ok &= Expect(!SecureDirTree(child_dir).ok, "insecure ancestor directory is rejected");
	const auto child_file = child_dir / "config.toml";
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

	const auto create_root = temp_root / "create-tree";
	const auto create_leaf = create_root / "sub" / "leaf";
	const howdy::native::file_security_internal::ValidationRoot create_boundary{temp_root};

	const auto created = howdy::native::CreateSecureDirectoryTree(
	    create_leaf, "Test directory", 0750, 0700, geteuid(), create_boundary);
	ok &= Expect(created.ok, "CreateSecureDirectoryTree creates nested tree");

	struct stat leaf_stat{};
	struct stat sub_stat{};
	ok &= Expect(stat(create_leaf.c_str(), &leaf_stat) == 0, "stat created leaf directory");
	ok &= Expect(stat((create_root / "sub").c_str(), &sub_stat) == 0, "stat created sub directory");
	ok &= Expect(howdy::native::MaskedBits(leaf_stat.st_mode, 0777) == 0750,
	             "created leaf mode is 0750");
	ok &= Expect(howdy::native::MaskedBits(sub_stat.st_mode, 0777) == 0700,
	             "created sub ancestor mode is 0700");

	const auto recreated = howdy::native::CreateSecureDirectoryTree(
	    create_leaf, "Test directory", 0750, 0700, geteuid(), create_boundary);
	ok &= Expect(recreated.ok, "CreateSecureDirectoryTree is idempotent on existing tree");

	const auto bad_root = temp_root / "bad-tree";
	const auto bad_leaf = bad_root / "sub" / "leaf";
	ok &= Expect(
	    CreateSecureDirectoryTree(bad_root, "Bad directory", 0750, 0700, geteuid(), create_boundary)
	        .ok,
	    "create tree root for insecure component cases");
	fs::create_directory_symlink(real_dir, bad_root / "sub", ec);
	ok &= Expect(!ec, "replace intermediate component with symlink");
	ok &= Expect(!CreateSecureDirectoryTree(bad_leaf, "Bad directory", 0750, 0700, geteuid(),
	                                        create_boundary)
	                  .ok,
	             "CreateSecureDirectoryTree rejects existing symlink component");
	ok &= Expect(!fs::exists(real_dir / "leaf", ec), "symlinked component is not populated");

	ok &= Expect(chmod((create_root / "sub").c_str(), 0777) == 0, "make intermediate insecure");
	ok &= Expect(!CreateSecureDirectoryTree(create_leaf, "Bad directory", 0750, 0700, geteuid(),
	                                        create_boundary)
	                  .ok,
	             "CreateSecureDirectoryTree rejects insecure existing intermediate");
	ok &= Expect(chmod((create_root / "sub").c_str(), 0700) == 0, "restore intermediate");
	ok &= Expect(chmod(create_leaf.c_str(), 0777) == 0, "make leaf insecure");
	ok &= Expect(!CreateSecureDirectoryTree(create_leaf, "Bad directory", 0750, 0700, geteuid(),
	                                        create_boundary)
	                  .ok,
	             "CreateSecureDirectoryTree rejects insecure existing leaf");
	ok &= Expect(chmod(create_leaf.c_str(), 0750) == 0, "restore leaf");

	ok &= Expect(!CreateSecureDirectoryTree("relative/tree", "Relative directory", 0750, 0700,
	                                        geteuid(), create_boundary)
	                  .ok,
	             "CreateSecureDirectoryTree rejects relative target");

	const auto outside_result = howdy::native::CreateSecureDirectoryTree(
	    "/tmp/outside-tree", "Outside directory", 0750, 0700, geteuid(), create_boundary);
	ok &= Expect(!outside_result.ok, "CreateSecureDirectoryTree rejects target outside boundary");

	const auto link_root = temp_root / "link-root";
	fs::create_directory_symlink(create_root, link_root, ec);
	const auto link_result = howdy::native::CreateSecureDirectoryTree(
	    link_root / "x", "Link directory", 0750, 0700, geteuid(),
	    howdy::native::file_security_internal::ValidationRoot{link_root});
	ok &= Expect(!ec && !link_result.ok, "CreateSecureDirectoryTree rejects symlink boundary root");

	fs::remove_all(temp_root, ec);

	return ok ? 0 : 1;
}
