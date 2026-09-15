#include "auth_helper/auth_helper_acl_fake.hpp"
#include "auth_helper/auth_helper_acl_policy.hpp"
#include "auth_helper/auth_helper_acl_probe.hpp"
#include "auth_helper/auth_helper_test_groups.hpp"
#include "auth_helper/auth_helper_test_io.hpp"
#include "auth_helper/runtime/internal.hpp"
#include "test_support.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>

namespace {
	using namespace howdy::test::auth_helper;
	using howdy::test::ScopedFd;

	auto ExpectAclProbeClassification(const std::filesystem::path &temp_root) -> bool {
		std::ostringstream output;
		auto              *previous_cerr = std::cerr.rdbuf(output.rdbuf());
		const auto         eopnotsupp    = ProbeAclSupport(temp_root, EOPNOTSUPP);
		const auto         enotsupp      = ProbeAclSupport(temp_root, ENOTSUP);
		const auto         eio           = ProbeAclSupport(temp_root, EIO);
		std::cerr.rdbuf(previous_cerr);
		bool ok = true;
		ok &= Expect(eopnotsupp == AclSupport::kUnsupported,
		             "EOPNOTSUPP ACL probe result skips ACL tests");
		ok &= Expect(enotsupp == AclSupport::kUnsupported,
		             "ENOTSUP ACL probe result skips ACL tests");
		ok &= Expect(eio == AclSupport::kError, "unexpected ACL probe error fails suite");
		return ok;
	}

	auto ExpectRuntimeRootValidation(const std::filesystem::path &temp_root) -> bool {
		using howdy::native::auth_helper::RuntimeRoot;
		using howdy::native::auth_helper::internal::ValidateRuntimeRoot;

		bool ok = true;
		ok &= Expect(RuntimeRoot() == "/run/howdy", "runtime root is fixed under /run/howdy");

		const auto regular_path = temp_root / "runtime-root-file";
		ok &= Expect(WriteFile(regular_path, "not a directory"), "writes runtime root file");
		ok &= Expect(!ValidateRuntimeRoot(regular_path, 0, 0),
		             "regular runtime root path is rejected");

		const auto      user_owned_dir = temp_root / "runtime-root-dir";
		std::error_code ec;
		std::filesystem::create_directory(user_owned_dir, ec);
		ok &= Expect(!ec, "creates runtime root directory fixture");
		ok &= Expect(chmod(user_owned_dir.c_str(), 0711) == 0, "sets runtime root fixture mode");

		const auto missing_dir = temp_root / "runtime-root-missing";
		ok &= Expect(!std::filesystem::exists(missing_dir),
		             "runtime root creation path starts missing");
		ok &= Expect(ValidateRuntimeRoot(missing_dir, geteuid(), getegid()),
		             "missing runtime root is created for current effective identity");
		struct stat missing_stat{};
		ok &= Expect(lstat(missing_dir.c_str(), &missing_stat) == 0, "stats created runtime root");
		ok &= Expect(S_ISDIR(missing_stat.st_mode), "created runtime root is a directory");
		ok &= Expect(missing_stat.st_uid == geteuid() && missing_stat.st_gid == getegid(),
		             "created runtime root has requested effective owner");
		ok &= Expect((missing_stat.st_mode & (S_IWGRP | S_IWOTH)) == 0,
		             "created runtime root is not group or world writable");
		ok &=
		    Expect((missing_stat.st_mode & 0777) == 0711, "created runtime root has expected mode");

		if (geteuid() == 0) {
			ok &= Expect(ValidateRuntimeRoot(user_owned_dir, 0, 0),
			             "root-owned runtime root directory is accepted");
		} else {
			ok &= Expect(!ValidateRuntimeRoot(user_owned_dir, 0, 0),
			             "user-owned runtime root directory is rejected");
		}

		return ok;
	}

	auto ExpectSecureSourceFileStat(const std::filesystem::path &temp_root) -> bool {
		using howdy::native::auth_helper::internal::SecureSourceFileStat;

		bool ok = true;
		ok &= Expect(!SecureSourceFileStat(-1, "Invalid fd", 0), "invalid fd is rejected");

		ScopedFd dir_fd(open(temp_root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
		ok &= Expect(dir_fd.Get() >= 0, "opens directory fd");
		ok &= Expect(!SecureSourceFileStat(dir_fd.Get(), "Directory", 0),
		             "directory fd is rejected as source file");

		const auto regular_path = temp_root / "source-file";
		ok &= Expect(WriteFile(regular_path, "source"), "writes source file");
		ok &= Expect(chmod(regular_path.c_str(), 0664) == 0, "makes source group-writable");
		ScopedFd group_writable_fd(open(regular_path.c_str(), O_RDONLY | O_CLOEXEC));
		ok &= Expect(group_writable_fd.Get() >= 0, "opens group-writable source");
		ok &= Expect(!SecureSourceFileStat(group_writable_fd.Get(), "Group writable source", 0),
		             "group-writable source is rejected");
		group_writable_fd.Reset();

		ok &= Expect(chmod(regular_path.c_str(), 0666) == 0, "makes source world-writable");
		ScopedFd world_writable_fd(open(regular_path.c_str(), O_RDONLY | O_CLOEXEC));
		ok &= Expect(world_writable_fd.Get() >= 0, "opens world-writable source");
		struct stat world_stat{};
		ok &=
		    Expect(fstat(world_writable_fd.Get(), &world_stat) == 0, "stats world-writable source");
		ok &= Expect(!SecureSourceFileStat(world_writable_fd.Get(), "World writable source",
		                                   world_stat.st_uid),
		             "world-writable source is rejected");
		world_writable_fd.Reset();

		ok &= Expect(chmod(regular_path.c_str(), 0644) == 0, "restores source mode");
		const auto hard_link_path = temp_root / "source-file-hard-link";
		if (link(regular_path.c_str(), hard_link_path.c_str()) == 0) {
			ScopedFd hard_link_fd(open(regular_path.c_str(), O_RDONLY | O_CLOEXEC));
			ok &= Expect(hard_link_fd.Get() >= 0, "opens hard-linked source");
			struct stat hard_link_stat{};
			ok &=
			    Expect(fstat(hard_link_fd.Get(), &hard_link_stat) == 0, "stats hard-linked source");
			ok &= Expect(!SecureSourceFileStat(hard_link_fd.Get(), "Hard-linked source",
			                                   hard_link_stat.st_uid),
			             "hard-linked source is rejected");
			hard_link_fd.Reset();
			ok &= Expect(unlink(hard_link_path.c_str()) == 0, "removes hard-linked source fixture");
		} else {
			ok &= Expect(false, std::string("creates hard-linked source: ") + std::strerror(errno));
		}
		ScopedFd regular_fd(open(regular_path.c_str(), O_RDONLY | O_CLOEXEC));
		ok &= Expect(regular_fd.Get() >= 0, "opens regular source");
		if (geteuid() == 0) {
			ok &= Expect(SecureSourceFileStat(regular_fd.Get(), "Root source", 0),
			             "root-owned regular source is accepted");
		} else {
			ok &= Expect(!SecureSourceFileStat(regular_fd.Get(), "User source", 0),
			             "non-root-owned regular source is rejected");
		}

		return ok;
	}

	auto ExpectSourceModelReadiness(const std::filesystem::path &temp_root) -> bool {
		namespace fs = std::filesystem;
		using howdy::native::auth_helper::internal::SelectSourceModelPath;

		bool                    ok = true;
		std::error_code         ec;
		std::optional<fs::path> selected_path;
		const auto              missing_dir = temp_root / "missing-source-models";

		selected_path = temp_root / "unexpected.dat";
		ok &= Expect(
		    SelectSourceModelPath(missing_dir, "alice", std::nullopt, selected_path, {temp_root}),
		    "missing source models directory is treated as no staged model");
		ok &=
		    Expect(!selected_path.has_value(), "missing source models directory selects no model");

		const auto models_dir = temp_root / "source-models";
		const auto model_path = models_dir / "alice.dat";
		fs::create_directory(models_dir, ec);
		ok &= Expect(!ec, "creates source models directory");
		ok &= Expect(chmod(models_dir.c_str(), 0755) == 0, "secures source models directory");

		selected_path = temp_root / "unexpected.dat";
		ok &= Expect(!SelectSourceModelPath(models_dir, "../alice", std::nullopt, selected_path,
		                                    {temp_root}),
		             "invalid source model user fails closed");
		ok &= Expect(!selected_path.has_value(),
		             "invalid source model user clears stale selected path");

		selected_path = temp_root / "unexpected.dat";
		ok &= Expect(
		    SelectSourceModelPath(models_dir, "alice", std::nullopt, selected_path, {temp_root}),
		    "missing source model file preserves prepare behavior");
		ok &= Expect(!selected_path.has_value(), "missing source model file selects no model");

		ok &= Expect(WriteFile(model_path, "not-json"), "writes secure source model");
		ok &= Expect(chmod(model_path.c_str(), 0644) == 0, "secures source model file");
		ok &= Expect(
		    SelectSourceModelPath(models_dir, "alice", std::nullopt, selected_path, {temp_root}),
		    "secure source model is accepted without parsing");
		ok &= Expect(selected_path == model_path, "secure source model path is selected");

		const auto relative_models_dir = fs::relative(models_dir, fs::current_path(), ec);
		ok &= Expect(!ec, "resolves relative source models directory");
		selected_path = temp_root / "unexpected.dat";
		ok &= Expect(SelectSourceModelPath(relative_models_dir, "alice", geteuid(), selected_path),
		             "secure source model is accepted with calling uid owner check");
		ok &=
		    Expect(selected_path.has_value(), "calling uid owner check selects source model path");
		ok &= Expect(selected_path.has_value() && selected_path->filename() == "alice.dat",
		             "calling uid owner check selects source model filename");
		ok &= Expect(selected_path.has_value() && fs::equivalent(*selected_path, model_path, ec),
		             "calling uid owner check selects equivalent source model path");
		ec.clear();

		struct stat model_stat{};
		ok &= Expect(lstat(model_path.c_str(), &model_stat) == 0, "stats secure source model");
		const uid_t mismatched_owner =
		    model_stat.st_uid == 0 ? static_cast<uid_t>(1) : static_cast<uid_t>(0);
		selected_path = temp_root / "unexpected.dat";
		ok &= Expect(
		    !SelectSourceModelPath(relative_models_dir, "alice", mismatched_owner, selected_path),
		    "secure source model with mismatched owner uid fails closed");
		ok &= Expect(!selected_path.has_value(), "mismatched owner uid clears stale selected path");

		if (geteuid() == 0) {
			selected_path = temp_root / "unexpected.dat";
			ok &= Expect(SelectSourceModelPath(relative_models_dir, "alice", static_cast<uid_t>(0),
			                                   selected_path),
			             "secure source model is accepted with root owner check");
			ok &= Expect(selected_path.has_value(), "root owner check selects source model path");
			ok &= Expect(selected_path.has_value() && selected_path->filename() == "alice.dat",
			             "root owner check selects source model filename");
			ok &=
			    Expect(selected_path.has_value() && fs::equivalent(*selected_path, model_path, ec),
			           "root owner check selects equivalent source model path");
			ec.clear();
		}

		const auto symlink_target = models_dir / "target.dat";
		ok &= Expect(WriteFile(symlink_target, "target"), "writes source model symlink target");
		fs::remove(model_path, ec);
		ec.clear();
		if (symlink(symlink_target.c_str(), model_path.c_str()) == 0) {
			selected_path = temp_root / "unexpected.dat";
			ok &= Expect(!SelectSourceModelPath(models_dir, "alice", std::nullopt, selected_path,
			                                    {temp_root}),
			             "symlinked source model file fails closed");
			ok &= Expect(!selected_path.has_value(),
			             "symlinked source model file clears stale selected path");
			fs::remove(model_path, ec);
			ec.clear();
		} else {
			std::cerr << "SKIP: source model symlink creation failed: " << std::strerror(errno)
			          << "\n";
		}

		const auto real_models_dir    = temp_root / "real-source-models";
		const auto symlink_models_dir = temp_root / "symlink-source-models";
		fs::create_directory(real_models_dir, ec);
		ok &= Expect(!ec, "creates real source models directory");
		ok &= Expect(chmod(real_models_dir.c_str(), 0755) == 0,
		             "secures real source models directory");
		if (symlink(real_models_dir.c_str(), symlink_models_dir.c_str()) == 0) {
			selected_path = temp_root / "unexpected.dat";
			ok &= Expect(!SelectSourceModelPath(symlink_models_dir, "alice", std::nullopt,
			                                    selected_path, {temp_root}),
			             "symlinked source models directory fails closed");
			ok &= Expect(!selected_path.has_value(),
			             "symlinked source models directory clears stale selected path");
			fs::remove(symlink_models_dir, ec);
			ec.clear();
		} else {
			std::cerr << "SKIP: source models directory symlink creation failed: "
			          << std::strerror(errno) << "\n";
		}

		ok &= Expect(WriteFile(model_path, "not-json"), "restores secure source model");
		ok &=
		    Expect(chmod(model_path.c_str(), 0664) == 0, "makes source model file group-writable");
		selected_path = temp_root / "unexpected.dat";
		ok &= Expect(
		    !SelectSourceModelPath(models_dir, "alice", std::nullopt, selected_path, {temp_root}),
		    "group-writable source model file fails closed");
		ok &= Expect(!selected_path.has_value(),
		             "group-writable source model file clears stale selected path");
		ok &=
		    Expect(chmod(model_path.c_str(), 0666) == 0, "makes source model file world-writable");
		ok &= Expect(
		    !SelectSourceModelPath(models_dir, "alice", std::nullopt, selected_path, {temp_root}),
		    "world-writable source model file fails closed");
		ok &= Expect(chmod(model_path.c_str(), 0644) == 0, "restores source model file mode");

		ok &= Expect(chmod(models_dir.c_str(), 0775) == 0,
		             "makes source models directory group-writable");
		selected_path = temp_root / "unexpected.dat";
		ok &= Expect(
		    !SelectSourceModelPath(models_dir, "alice", std::nullopt, selected_path, {temp_root}),
		    "group-writable source models directory fails closed");
		ok &= Expect(!selected_path.has_value(),
		             "group-writable source models directory clears stale selected path");
		ok &= Expect(chmod(models_dir.c_str(), 0777) == 0,
		             "makes source models directory world-writable");
		ok &= Expect(
		    !SelectSourceModelPath(models_dir, "alice", std::nullopt, selected_path, {temp_root}),
		    "world-writable source models directory fails closed");
		ok &= Expect(chmod(models_dir.c_str(), 0755) == 0, "restores source models directory mode");

		return ok;
	}

}  // namespace

auto RunAuthHelperPathTests(const std::filesystem::path &temp_root) -> bool {
	bool ok = true;
	ok &= ExpectAclProbeClassification(temp_root);
	ok &= ExpectRuntimeRootValidation(temp_root);
	ok &= ExpectSecureSourceFileStat(temp_root);
	ok &= ExpectSourceModelReadiness(temp_root);
	return ok;
}
