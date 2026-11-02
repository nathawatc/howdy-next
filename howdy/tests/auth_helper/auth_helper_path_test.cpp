#include "auth_helper/auth_helper_acl_probe.hpp"
#include "auth_helper/auth_helper_test_groups.hpp"
#include "auth_helper/command.hpp"
#include "auth_helper/runtime.hpp"
#include "auth_helper/runtime/internal.hpp"
#include "support/bit_flags.hpp"
#include "support/scoped_stream_capture.hpp"
#include "test_support.hpp"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

#include <sys/stat.h>

namespace {
	using howdy::test::Expect;
	using howdy::test::ScopedFd;
	using howdy::test::WriteFile;
	using howdy::test::auth_helper::AclSupport;
	using howdy::test::auth_helper::ProbeAclSupport;

	auto ExpectAclProbeClassification(const std::filesystem::path &temp_root) -> bool {
		std::ostringstream const output;
		auto                    *previous_cerr = std::cerr.rdbuf(output.rdbuf());
		const auto               eopnotsupp    = ProbeAclSupport(temp_root, EOPNOTSUPP);
		const auto               enotsupp      = ProbeAclSupport(temp_root, ENOTSUP);
		const auto               eio           = ProbeAclSupport(temp_root, EIO);
		std::cerr.rdbuf(previous_cerr);
		bool ok = true;
		ok &= Expect(eopnotsupp == AclSupport::kUnsupported,
		             "EOPNOTSUPP ACL probe result skips ACL tests");
		ok &= Expect(enotsupp == AclSupport::kUnsupported,
		             "ENOTSUP ACL probe result skips ACL tests");
		ok &= Expect(eio == AclSupport::kError, "unexpected ACL probe error fails suite");
		return ok;
	}

	auto ExpectQuietDiagnostics() -> bool {
		using howdy::native::ScopedStreamCapture;
		using howdy::native::auth_helper::command::RunWithQuietDiagnostics;

		const auto *const original = std::cerr.rdbuf();
		bool              ok       = true;
		bool              unwound  = false;
		try {
			const ScopedStreamCapture capture(std::cerr);
			throw std::runtime_error("unwind");
		} catch (const std::runtime_error &) {
			unwound = true;
		}
		ok &= Expect(unwound && std::cerr.rdbuf() == original,
		             "stream capture restores cerr while unwinding");

		std::string reported;
		const auto  run = [&](const std::function<bool()> &operation) -> std::string {
			reported.clear();
			ScopedStreamCapture outer(std::cerr);
			try {
				(void)RunWithQuietDiagnostics(operation, [&](std::string_view line) -> void {
					reported += std::string(line) + "|";
				});
			} catch (const std::runtime_error &) {
				std::cerr << "[rethrown]";
			}
			return outer.Release();
		};
		ok &= Expect(run([] -> bool {
			             std::cerr << "slot diagnostic\n";
			             return true;
		             }).empty() &&
		                 reported == "slot diagnostic|",
		             "diagnostics are logged, not written to stderr, when the operation succeeds");
		ok &= Expect(run([&] -> bool {
			             std::cerr << "first\nsecond";
			             const bool logged_live = reported == "first|";
			             std::cerr << "\n";
			             return !logged_live;
		             }) == "first\nsecond\n" &&
		                 reported == "first|second|",
		             "each line is logged as it is written, before the operation returns");
		ok &= Expect(run([] -> bool {
			             std::cerr << "no newline";
			             return true;
		             }).empty() &&
		                 reported == "no newline|",
		             "a last line without a newline is still logged on success");
		ok &= Expect(run([] -> bool {
			             std::cerr << "slot diagnostic\n";
			             throw std::runtime_error("prepare failed");
		             }) == "slot diagnostic\n[rethrown]" &&
		                 reported == "slot diagnostic|",
		             "diagnostics reach stderr before an exception propagates");
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
		// Use the directory's real owner: a setgid parent can give it a gid other than getegid().
		// Only the gid differs in the wrong-group case, and an existing root is never chowned, so
		// it needs no root.
		struct stat owned_stat{};
		ok &= Expect(lstat(user_owned_dir.c_str(), &owned_stat) == 0, "stats runtime root fixture");
		ok &= Expect(ValidateRuntimeRoot(user_owned_dir, owned_stat.st_uid, owned_stat.st_gid),
		             "existing runtime root with its own owner and group is accepted");
		ok &= Expect(!ValidateRuntimeRoot(user_owned_dir, owned_stat.st_uid, owned_stat.st_gid + 1),
		             "existing runtime root with a wrong group is rejected");

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
		ok &= Expect(!howdy::native::HasAnyBits(missing_stat.st_mode, S_IWGRP, S_IWOTH),
		             "created runtime root is not group or world writable");
		ok &= Expect(howdy::native::MaskedBits(missing_stat.st_mode, 0777) == 0711,
		             "created runtime root has expected mode");

		for (const mode_t bad_mode : {mode_t{0777}, mode_t{0755}}) {
			ok &=
			    Expect(chmod(user_owned_dir.c_str(), bad_mode) == 0, "sets bad runtime root mode");
			ok &= Expect(!ValidateRuntimeRoot(user_owned_dir, owned_stat.st_uid, owned_stat.st_gid),
			             "existing runtime root with a wrong mode is rejected");
		}
		ok &= Expect(chmod(user_owned_dir.c_str(), 0711) == 0, "restores runtime root mode");
		ok &= Expect(ValidateRuntimeRoot(user_owned_dir, owned_stat.st_uid, owned_stat.st_gid),
		             "existing runtime root with the exact owner and mode is accepted");

		const auto symlink_path = temp_root / "runtime-root-symlink";
		ok &= Expect(symlink(user_owned_dir.c_str(), symlink_path.c_str()) == 0,
		             "creates runtime root symlink fixture");
		ok &= Expect(!ValidateRuntimeRoot(symlink_path, owned_stat.st_uid, owned_stat.st_gid),
		             "symlinked runtime root is rejected");

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
		struct stat dir_stat{};
		ok &= Expect(fstat(dir_fd.Get(), &dir_stat) == 0, "stats directory fd");
		ok &= Expect(!SecureSourceFileStat(dir_fd.Get(), "Directory", dir_stat.st_uid),
		             "directory fd is rejected as source file");

		const auto regular_path = temp_root / "source-file";
		ok &= Expect(WriteFile(regular_path, "source"), "writes source file");
		ok &= Expect(chmod(regular_path.c_str(), 0664) == 0, "makes source group-writable");
		ScopedFd group_writable_fd(open(regular_path.c_str(), O_RDONLY | O_CLOEXEC));
		ok &= Expect(group_writable_fd.Get() >= 0, "opens group-writable source");
		struct stat group_stat{};
		ok &=
		    Expect(fstat(group_writable_fd.Get(), &group_stat) == 0, "stats group-writable source");
		ok &= Expect(!SecureSourceFileStat(group_writable_fd.Get(), "Group writable source",
		                                   group_stat.st_uid),
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
	ok &= ExpectQuietDiagnostics();
	ok &= ExpectRuntimeRootValidation(temp_root);
	ok &= ExpectSecureSourceFileStat(temp_root);
	ok &= ExpectSourceModelReadiness(temp_root);
	return ok;
}
