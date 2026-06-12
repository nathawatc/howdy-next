#include "auth_helper_runtime.hpp"
#include "auth_helper_testing.hpp"
#include "common/auth_helper_protocol.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>

namespace {

	class ScopedFd {
	public:
		ScopedFd() = default;

		explicit ScopedFd(int fd)
		    : fd_(fd) {}

		ScopedFd(const ScopedFd &)                     = delete;
		auto operator=(const ScopedFd &) -> ScopedFd & = delete;

		ScopedFd(ScopedFd &&other) noexcept
		    : fd_(other.release()) {}

		auto operator=(ScopedFd &&other) noexcept -> ScopedFd & {
			if (this != &other) {
				reset(other.release());
			}
			return *this;
		}

		~ScopedFd() {
			reset();
		}

		[[nodiscard]] auto get() const -> int {
			return fd_;
		}

		void reset(int fd = -1) {
			if (fd_ >= 0) {
				close(fd_);
			}
			fd_ = fd;
		}

		auto release() -> int {
			const int fd = fd_;
			fd_          = -1;
			return fd;
		}

	private:
		int fd_ = -1;
	};

	auto expect(bool condition, const std::string &message) -> bool {
		if (!condition) {
			std::cerr << "FAIL: " << message << "\n";
			return false;
		}
		return true;
	}

	auto write_file(const std::filesystem::path &path, const std::string &content) -> bool {
		std::ofstream output(path);
		output << content;
		return output.good();
	}

	auto read_file(const std::filesystem::path &path) -> std::string {
		std::ifstream input(path);
		return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	}

	auto runtime_dirs_for_uid(const std::filesystem::path &runtime_root, uid_t uid)
	    -> std::set<std::filesystem::path> {
		namespace fs = std::filesystem;

		std::set<fs::path> paths;
		std::error_code    ec;
		const auto         prefix = "pam-" + std::to_string(uid) + "-";
		for (const auto &entry : fs::directory_iterator(runtime_root, ec)) {
			if (entry.path().filename().string().starts_with(prefix)) {
				paths.insert(entry.path());
			}
		}
		return paths;
	}

	auto expect_runtime_root_validation(const std::filesystem::path &temp_root) -> bool {
		using howdy::native::testing::runtime_root;
		using howdy::native::testing::validate_runtime_root;

		bool ok = true;
		ok &= expect(runtime_root() == "/run/howdy", "runtime root is fixed under /run/howdy");

		const auto regular_path = temp_root / "runtime-root-file";
		ok &= expect(write_file(regular_path, "not a directory"), "writes runtime root file");
		ok &= expect(!validate_runtime_root(regular_path), "regular runtime root path is rejected");

		const auto      user_owned_dir = temp_root / "runtime-root-dir";
		std::error_code ec;
		std::filesystem::create_directory(user_owned_dir, ec);
		ok &= expect(!ec, "creates runtime root directory fixture");
		if (geteuid() == 0) {
			ok &= expect(validate_runtime_root(user_owned_dir),
			             "root-owned runtime root directory is accepted");
		} else {
			ok &= expect(!validate_runtime_root(user_owned_dir),
			             "user-owned runtime root directory is rejected");
		}

		return ok;
	}

	auto expect_secure_source_file_stat(const std::filesystem::path &temp_root) -> bool {
		using howdy::native::testing::secure_source_file_stat;

		bool ok = true;
		ok &= expect(!secure_source_file_stat(-1, "Invalid fd"), "invalid fd is rejected");

		ScopedFd dir_fd(open(temp_root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
		ok &= expect(dir_fd.get() >= 0, "opens directory fd");
		ok &= expect(!secure_source_file_stat(dir_fd.get(), "Directory"),
		             "directory fd is rejected as source file");

		const auto regular_path = temp_root / "source-file";
		ok &= expect(write_file(regular_path, "source"), "writes source file");
		ok &= expect(chmod(regular_path.c_str(), 0664) == 0, "makes source group-writable");
		ScopedFd group_writable_fd(open(regular_path.c_str(), O_RDONLY | O_CLOEXEC));
		ok &= expect(group_writable_fd.get() >= 0, "opens group-writable source");
		ok &= expect(!secure_source_file_stat(group_writable_fd.get(), "Group writable source"),
		             "group-writable source is rejected");
		group_writable_fd.reset();

		ok &= expect(chmod(regular_path.c_str(), 0644) == 0, "restores source mode");
		ScopedFd regular_fd(open(regular_path.c_str(), O_RDONLY | O_CLOEXEC));
		ok &= expect(regular_fd.get() >= 0, "opens regular source");
		if (geteuid() == 0) {
			ok &= expect(secure_source_file_stat(regular_fd.get(), "Root source"),
			             "root-owned regular source is accepted");
		} else {
			ok &= expect(!secure_source_file_stat(regular_fd.get(), "User source"),
			             "non-root-owned regular source is rejected");
		}

		return ok;
	}

	auto expect_write_all_helper(const std::filesystem::path &temp_root) -> bool {
		using howdy::native::testing::write_all;

		bool ok = true;
		ok &= expect(!write_all(-1, "x", 1), "invalid write fd is rejected");

		const auto output_path = temp_root / "write-all-output";
		ScopedFd   output_fd(
		    open(output_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
		ok &= expect(output_fd.get() >= 0, "creates write-all output file");
		constexpr auto kOutput = "alpha\nbeta\n";
		ok &=
		    expect(write_all(output_fd.get(), kOutput, static_cast<ssize_t>(std::strlen(kOutput))),
		           "writes complete buffer");
		output_fd.reset();
		ok &= expect(read_file(output_path) == kOutput, "write-all output matches input");

		return ok;
	}

	auto expect_copy_file_for_user(const std::filesystem::path &temp_root) -> bool {
		using howdy::native::testing::copy_file_for_user;

		bool ok = true;
		ok &= expect(
		    !copy_file_for_user(temp_root / "missing", temp_root / "dest", "Missing", getgid()),
		    "missing source copy fails");

		const auto real_source    = temp_root / "real-source";
		const auto symlink_source = temp_root / "symlink-source";
		ok &= expect(write_file(real_source, "real"), "writes real source");
		if (symlink(real_source.c_str(), symlink_source.c_str()) == 0) {
			ok &= expect(!copy_file_for_user(symlink_source, temp_root / "symlink-dest", "Symlink",
			                                 getgid()),
			             "symlink source copy fails closed");
		} else {
			std::cerr << "SKIP: symlink source creation failed: " << std::strerror(errno) << "\n";
		}

		if (geteuid() == 0) {
			const auto destination = temp_root / "copied-source";
			ok &= expect(chmod(real_source.c_str(), 0644) == 0, "sets secure source mode");
			ok &= expect(copy_file_for_user(real_source, destination, "Source", getgid()),
			             "secure source copy succeeds as root");
			ok &= expect(read_file(destination) == "real", "copied file preserves content");

			struct stat stat_{};
			ok &= expect(lstat(destination.c_str(), &stat_) == 0, "stats copied file");
			ok &= expect(stat_.st_uid == 0 && stat_.st_gid == getgid(),
			             "copied file owner is root and invoking group");
			ok &= expect((stat_.st_mode & 0777) == 0440, "copied file mode is restricted");
			ok &= expect(!copy_file_for_user(real_source, destination, "Existing", getgid()),
			             "existing destination copy fails");
		} else {
			std::cerr << "SKIP: successful auth-helper copy requires root-owned source\n";
		}

		return ok;
	}

	auto expect_source_model_readiness(const std::filesystem::path &temp_root) -> bool {
		namespace fs = std::filesystem;
		using howdy::native::testing::copy_file_for_user;
		using howdy::native::testing::select_source_model_path;

		bool                    ok = true;
		std::error_code         ec;
		std::optional<fs::path> selected_path;
		const auto              missing_dir = temp_root / "missing-source-models";

		selected_path = temp_root / "unexpected.dat";
		ok &= expect(select_source_model_path(missing_dir, "alice", std::nullopt, selected_path),
		             "missing source models directory is treated as no staged model");
		ok &=
		    expect(!selected_path.has_value(), "missing source models directory selects no model");

		const auto models_dir = temp_root / "source-models";
		const auto model_path = models_dir / "alice.dat";
		fs::create_directory(models_dir, ec);
		ok &= expect(!ec, "creates source models directory");
		ok &= expect(chmod(models_dir.c_str(), 0755) == 0, "secures source models directory");

		selected_path = temp_root / "unexpected.dat";
		ok &= expect(!select_source_model_path(models_dir, "../alice", std::nullopt, selected_path),
		             "invalid source model user fails closed");
		ok &= expect(!selected_path.has_value(),
		             "invalid source model user clears stale selected path");

		selected_path = temp_root / "unexpected.dat";
		ok &= expect(select_source_model_path(models_dir, "alice", std::nullopt, selected_path),
		             "missing source model file preserves prepare behavior");
		ok &= expect(!selected_path.has_value(), "missing source model file selects no model");

		ok &= expect(write_file(model_path, "not-json"), "writes secure source model");
		ok &= expect(chmod(model_path.c_str(), 0644) == 0, "secures source model file");
		ok &= expect(select_source_model_path(models_dir, "alice", std::nullopt, selected_path),
		             "secure source model is accepted without parsing");
		ok &= expect(selected_path == model_path, "secure source model path is selected");

		const auto relative_models_dir = fs::relative(models_dir, fs::current_path(), ec);
		ok &= expect(!ec, "resolves relative source models directory");
		selected_path = temp_root / "unexpected.dat";
		ok &=
		    expect(select_source_model_path(relative_models_dir, "alice", geteuid(), selected_path),
		           "secure source model is accepted with calling uid owner check");
		ok &=
		    expect(selected_path.has_value(), "calling uid owner check selects source model path");
		ok &= expect(selected_path.has_value() && selected_path->filename() == "alice.dat",
		             "calling uid owner check selects source model filename");
		ok &= expect(selected_path.has_value() && fs::equivalent(*selected_path, model_path, ec),
		             "calling uid owner check selects equivalent source model path");
		ec.clear();

		struct stat model_stat{};
		ok &= expect(lstat(model_path.c_str(), &model_stat) == 0, "stats secure source model");
		const uid_t mismatched_owner =
		    model_stat.st_uid == 0 ? static_cast<uid_t>(1) : static_cast<uid_t>(0);
		selected_path = temp_root / "unexpected.dat";
		ok &= expect(!select_source_model_path(relative_models_dir, "alice", mismatched_owner,
		                                       selected_path),
		             "secure source model with mismatched owner uid fails closed");
		ok &= expect(!selected_path.has_value(), "mismatched owner uid clears stale selected path");

		if (geteuid() == 0) {
			selected_path = temp_root / "unexpected.dat";
			ok &= expect(select_source_model_path(relative_models_dir, "alice",
			                                      static_cast<uid_t>(0), selected_path),
			             "secure source model is accepted with root owner check");
			ok &= expect(selected_path.has_value(), "root owner check selects source model path");
			ok &= expect(selected_path.has_value() && selected_path->filename() == "alice.dat",
			             "root owner check selects source model filename");
			ok &=
			    expect(selected_path.has_value() && fs::equivalent(*selected_path, model_path, ec),
			           "root owner check selects equivalent source model path");
			ec.clear();
		}

		if (geteuid() == 0) {
			const auto staged_path = temp_root / "staged-alice.dat";
			ok &= expect(copy_file_for_user(model_path, staged_path, "User model file", getgid()),
			             "secure source model is staged");
			ok &= expect(fs::exists(staged_path), "staged source model exists");
		} else {
			std::cerr << "SKIP: successful auth-helper staging requires root-owned source\n";
		}

		const auto symlink_target = models_dir / "target.dat";
		ok &= expect(write_file(symlink_target, "target"), "writes source model symlink target");
		fs::remove(model_path, ec);
		ec.clear();
		if (symlink(symlink_target.c_str(), model_path.c_str()) == 0) {
			selected_path = temp_root / "unexpected.dat";
			ok &=
			    expect(!select_source_model_path(models_dir, "alice", std::nullopt, selected_path),
			           "symlinked source model file fails closed");
			ok &= expect(!selected_path.has_value(),
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
		ok &= expect(!ec, "creates real source models directory");
		ok &= expect(chmod(real_models_dir.c_str(), 0755) == 0,
		             "secures real source models directory");
		if (symlink(real_models_dir.c_str(), symlink_models_dir.c_str()) == 0) {
			selected_path = temp_root / "unexpected.dat";
			ok &= expect(
			    !select_source_model_path(symlink_models_dir, "alice", std::nullopt, selected_path),
			    "symlinked source models directory fails closed");
			ok &= expect(!selected_path.has_value(),
			             "symlinked source models directory clears stale selected path");
			fs::remove(symlink_models_dir, ec);
			ec.clear();
		} else {
			std::cerr << "SKIP: source models directory symlink creation failed: "
			          << std::strerror(errno) << "\n";
		}

		ok &= expect(write_file(model_path, "not-json"), "restores secure source model");
		ok &=
		    expect(chmod(model_path.c_str(), 0664) == 0, "makes source model file group-writable");
		selected_path = temp_root / "unexpected.dat";
		ok &= expect(!select_source_model_path(models_dir, "alice", std::nullopt, selected_path),
		             "group-writable source model file fails closed");
		ok &= expect(!selected_path.has_value(),
		             "group-writable source model file clears stale selected path");
		ok &=
		    expect(chmod(model_path.c_str(), 0666) == 0, "makes source model file world-writable");
		ok &= expect(!select_source_model_path(models_dir, "alice", std::nullopt, selected_path),
		             "world-writable source model file fails closed");
		ok &= expect(chmod(model_path.c_str(), 0644) == 0, "restores source model file mode");

		ok &= expect(chmod(models_dir.c_str(), 0775) == 0,
		             "makes source models directory group-writable");
		selected_path = temp_root / "unexpected.dat";
		ok &= expect(!select_source_model_path(models_dir, "alice", std::nullopt, selected_path),
		             "group-writable source models directory fails closed");
		ok &= expect(!selected_path.has_value(),
		             "group-writable source models directory clears stale selected path");
		ok &= expect(chmod(models_dir.c_str(), 0777) == 0,
		             "makes source models directory world-writable");
		ok &= expect(!select_source_model_path(models_dir, "alice", std::nullopt, selected_path),
		             "world-writable source models directory fails closed");
		ok &= expect(chmod(models_dir.c_str(), 0755) == 0, "restores source models directory mode");

		return ok;
	}

	auto expect_prepare_cleanup_guards() -> bool {
		using howdy::native::testing::cleanup_for_user;
		using howdy::native::testing::prepare_for_user;

		bool ok = true;
		if (geteuid() == 0) {
			ok &= expect(prepare_for_user("../alice") == 1, "root prepare rejects invalid user");
			ok &= expect(cleanup_for_user("/tmp/not-howdy-runtime") == 1,
			             "root cleanup rejects unexpected path");
			const auto missing_expected = std::filesystem::path("/run/howdy") /
			                              ("pam-" + std::to_string(getuid()) + "-missing");
			ok &= expect(cleanup_for_user(missing_expected) == 0,
			             "root cleanup accepts missing expected runtime dir");
		} else {
			ok &= expect(prepare_for_user("../alice") == 1, "non-root prepare fails closed");
			ok &= expect(cleanup_for_user("/run/howdy/pam-0-missing") == 1,
			             "non-root cleanup fails closed");
		}

		return ok;
	}

	auto expect_cleanup_runtime_auth_files(const std::filesystem::path &temp_root) -> bool {
		namespace fs = std::filesystem;
		using howdy::native::auth_helper::cleanup_runtime_auth_files_for_test;

		bool            ok = true;
		std::error_code ec;
		const auto      fixture_root = temp_root / "cleanup-runtime-auth-files";
		const auto      runtime_root = fixture_root / "runtime-root";
		const auto      uid          = getuid();
		const auto      gid          = getgid();
		const auto      prefix       = "pam-" + std::to_string(uid) + "-";
		const uid_t     non_root_uid = 1;
		const gid_t     wrong_gid    = gid == 0 ? static_cast<gid_t>(1) : static_cast<gid_t>(0);

		fs::remove_all(fixture_root, ec);
		fs::create_directories(runtime_root, ec);
		ok &= expect(!ec, "creates cleanup runtime fixtures");
		ok &= expect(chmod(runtime_root.c_str(), 0711) == 0, "secures cleanup runtime root");

		const auto wrong_parent = fixture_root / "wrong-parent" / (prefix + "wrong-parent");
		ok &= expect(!cleanup_runtime_auth_files_for_test(wrong_parent, uid, gid, runtime_root).ok,
		             "wrong cleanup parent is rejected");

		const auto wrong_prefix = runtime_root / ("pam-" + std::to_string(uid + 1) + "-wrong");
		ok &= expect(!cleanup_runtime_auth_files_for_test(wrong_prefix, uid, gid, runtime_root).ok,
		             "wrong pam uid prefix is rejected");

		const auto missing_runtime_dir = runtime_root / (prefix + "missing");
		ok &= expect(
		    cleanup_runtime_auth_files_for_test(missing_runtime_dir, uid, gid, runtime_root).ok,
		    "missing expected runtime dir succeeds");
		ok &=
		    expect(!fs::exists(missing_runtime_dir), "missing expected runtime dir stays missing");

		const auto regular_file = runtime_root / (prefix + "regular");
		ok &= expect(write_file(regular_file, "cleanup"), "writes runtime cleanup file");
		ok &= expect(!cleanup_runtime_auth_files_for_test(regular_file, uid, gid, runtime_root).ok,
		             "regular file is rejected for cleanup");
		ok &= expect(fs::exists(regular_file), "regular file remains after rejected cleanup");
		fs::remove(regular_file, ec);
		ec.clear();

		const auto symlink_target = fixture_root / "cleanup-target";
		const auto symlink_path   = runtime_root / (prefix + "symlink");
		ok &= expect(write_file(symlink_target, "target"), "writes cleanup symlink target");
		if (symlink(symlink_target.c_str(), symlink_path.c_str()) == 0) {
			ok &= expect(
			    !cleanup_runtime_auth_files_for_test(symlink_path, uid, gid, runtime_root).ok,
			    "symlink is rejected for cleanup");
			ok &= expect(fs::exists(symlink_path), "symlink remains after rejected cleanup");
			fs::remove(symlink_path, ec);
			ec.clear();
		} else {
			std::cerr << "SKIP: cleanup symlink fixture creation failed: " << std::strerror(errno)
			          << "\n";
		}
		fs::remove(symlink_target, ec);
		ec.clear();

		const auto privileged_probe = fixture_root / "privileged-cleanup-probe";
		fs::create_directories(privileged_probe, ec);
		ok &= expect(!ec, "creates privileged cleanup probe");
		const bool can_setup_privileged_cleanup =
		    chown(privileged_probe.c_str(), non_root_uid, gid) == 0 &&
		    chown(privileged_probe.c_str(), 0, wrong_gid) == 0 &&
		    chown(privileged_probe.c_str(), 0, gid) == 0;
		(void)chown(privileged_probe.c_str(), uid, gid);
		std::error_code privileged_probe_cleanup_ec;
		fs::remove_all(privileged_probe, privileged_probe_cleanup_ec);
		const bool removed_privileged_probe = !privileged_probe_cleanup_ec;
		ec.clear();

		if (!can_setup_privileged_cleanup) {
			std::cerr << "SKIP: cleanup ownership/mode cases need required chown capabilities\n";
		} else {
			const bool privileged_probe_cleanup_ok =
			    expect(removed_privileged_probe, "removes privileged cleanup probe");
			ok &= privileged_probe_cleanup_ok;
			if (privileged_probe_cleanup_ok) {
				const auto group_writable_dir = runtime_root / (prefix + "group-writable");
				fs::create_directories(group_writable_dir, ec);
				ok &= expect(!ec, "creates group-writable cleanup directory");
				ok &= expect(chown(group_writable_dir.c_str(), 0, gid) == 0,
				             "sets root-owned group-writable cleanup directory");
				ok &= expect(chmod(group_writable_dir.c_str(), 0770) == 0,
				             "makes cleanup directory group-writable");
				ok &= expect(
				    !cleanup_runtime_auth_files_for_test(group_writable_dir, uid, gid, runtime_root)
				         .ok,
				    "group-writable directory is rejected");
				fs::remove_all(group_writable_dir, ec);
				ec.clear();

				const auto world_writable_dir = runtime_root / (prefix + "world-writable");
				fs::create_directories(world_writable_dir, ec);
				ok &= expect(!ec, "creates world-writable cleanup directory");
				ok &= expect(chown(world_writable_dir.c_str(), 0, gid) == 0,
				             "sets root-owned world-writable cleanup directory");
				ok &= expect(chmod(world_writable_dir.c_str(), 0777) == 0,
				             "makes cleanup directory world-writable");
				ok &= expect(
				    !cleanup_runtime_auth_files_for_test(world_writable_dir, uid, gid, runtime_root)
				         .ok,
				    "world-writable directory is rejected");
				fs::remove_all(world_writable_dir, ec);
				ec.clear();

				const auto valid_runtime_dir = runtime_root / (prefix + "valid");
				fs::create_directories(valid_runtime_dir, ec);
				ok &= expect(!ec, "creates valid cleanup directory");
				ok &= expect(chown(valid_runtime_dir.c_str(), 0, gid) == 0,
				             "sets root-owned valid cleanup directory");
				ok &= expect(chmod(valid_runtime_dir.c_str(), 0711) == 0,
				             "secures valid cleanup directory");
				ok &= expect(
				    cleanup_runtime_auth_files_for_test(valid_runtime_dir, uid, gid, runtime_root)
				        .ok,
				    "valid runtime directory is removed");
				ok &= expect(!fs::exists(valid_runtime_dir), "valid runtime directory is gone");

				const auto wrong_owner_dir = runtime_root / (prefix + "wrong-owner");
				fs::create_directories(wrong_owner_dir, ec);
				ok &= expect(!ec, "creates wrong-owner cleanup directory");
				ok &= expect(chown(wrong_owner_dir.c_str(), non_root_uid, gid) == 0,
				             "sets wrong-owner cleanup directory");
				ok &= expect(
				    !cleanup_runtime_auth_files_for_test(wrong_owner_dir, uid, gid, runtime_root)
				         .ok,
				    "wrong owner is rejected");
				fs::remove_all(wrong_owner_dir, ec);
				ec.clear();

				const auto wrong_gid_dir = runtime_root / (prefix + "wrong-gid");
				fs::create_directories(wrong_gid_dir, ec);
				ok &= expect(!ec, "creates wrong-gid cleanup directory");
				ok &= expect(chown(wrong_gid_dir.c_str(), 0, wrong_gid) == 0,
				             "sets wrong-gid cleanup directory");
				ok &= expect(
				    !cleanup_runtime_auth_files_for_test(wrong_gid_dir, uid, gid, runtime_root).ok,
				    "wrong gid is rejected");
				fs::remove_all(wrong_gid_dir, ec);
				ec.clear();
			}
		}

		fs::remove_all(fixture_root, ec);
		ok &= expect(!ec, "cleans cleanup runtime fixtures");
		return ok;
	}

	auto expect_prepare_runtime_auth_files(const std::filesystem::path &temp_root) -> bool {
		namespace fs = std::filesystem;
		using howdy::native::auth_helper::prepare_runtime_auth_files_for_test;

		bool            ok = true;
		std::error_code ec;
		const auto      fixture_dir  = temp_root / "prepare-runtime-auth-files";
		const auto      runtime_root = fixture_dir / "runtime-root";
		const auto      source_dir   = fixture_dir / "source";
		const auto      config_path  = fs::relative(source_dir / "config.ini", fs::current_path());
		const auto      models_dir   = fs::relative(source_dir / "models", fs::current_path());
		const auto      model_path   = models_dir / "alice.dat";
		const uid_t     owner_uid    = geteuid();
		fs::remove_all(fixture_dir, ec);
		fs::create_directories(runtime_root, ec);
		ok &= expect(!ec, "creates injected runtime root");
		ok &= expect(chmod(runtime_root.c_str(), 0711) == 0, "secures injected runtime root");
		fs::create_directories(models_dir, ec);
		ok &= expect(!ec, "creates prepare runtime auth files fixtures");
		ok &= expect(write_file(config_path, "config-content"), "writes secure source config");
		ok &= expect(chmod(fixture_dir.c_str(), 0755) == 0, "secures source fixture directory");
		ok &= expect(chmod(source_dir.c_str(), 0755) == 0, "secures source directory");
		ok &= expect(chmod(config_path.c_str(), 0644) == 0, "secures source config");
		ok &= expect(chmod(models_dir.c_str(), 0755) == 0, "secures source models directory");

		const auto before_success = runtime_dirs_for_uid(runtime_root, getuid());
		const auto prepared       = prepare_runtime_auth_files_for_test(
		    "alice", getuid(), getgid(), runtime_root, config_path, models_dir, owner_uid);
		ok &= expect(prepared.has_value(), "missing source user model still prepares auth files");
		if (prepared.has_value()) {
			const auto prefix = "pam-" + std::to_string(getuid()) + "-";
			ok &= expect(prepared->runtime_dir.parent_path() == runtime_root,
			             "prepared runtime directory uses injected runtime root");
			ok &= expect(prepared->runtime_dir.filename().string().starts_with(prefix),
			             "prepared runtime directory uses pam uid prefix");
			ok &= expect(prepared->config_path == prepared->runtime_dir / "config.ini",
			             "prepared config path uses runtime directory");
			ok &= expect(prepared->user_models_dir == prepared->runtime_dir / "models",
			             "prepared models path uses runtime directory");
			ok &= expect(fs::is_directory(prepared->runtime_dir),
			             "successful prepare creates runtime directory");
			ok &= expect(read_file(prepared->config_path) == "config-content",
			             "successful prepare stages config.ini");
			ok &= expect(fs::is_directory(prepared->user_models_dir),
			             "successful prepare creates models directory");
			ok &= expect(!fs::exists(prepared->user_models_dir / "alice.dat"),
			             "missing source user model remains unstaged");
			fs::remove_all(prepared->runtime_dir, ec);
			ok &= expect(!ec, "cleans successful prepared runtime directory");
		}
		ok &= expect(runtime_dirs_for_uid(runtime_root, getuid()) == before_success,
		             "successful prepare fixture leaves no runtime directory");

		const auto before_config_failure = runtime_dirs_for_uid(runtime_root, getuid());
		ok &= expect(!prepare_runtime_auth_files_for_test("alice", getuid(), getgid(), runtime_root,
		                                                  source_dir / "missing.ini", models_dir,
		                                                  owner_uid)
		                  .has_value(),
		             "failed config staging rejects prepare");
		ok &= expect(runtime_dirs_for_uid(runtime_root, getuid()) == before_config_failure,
		             "failed config staging removes private runtime directory");

		if (geteuid() == 0) {
			std::cerr << "SKIP: config copy permission failure requires non-root test process\n";
		} else {
			ok &= expect(chmod(config_path.c_str(), 0000) == 0, "makes source config unreadable");
			const auto before_config_copy_failure = runtime_dirs_for_uid(runtime_root, getuid());
			ok &= expect(!prepare_runtime_auth_files_for_test("alice", getuid(), getgid(),
			                                                  runtime_root, config_path, models_dir,
			                                                  owner_uid)
			                  .has_value(),
			             "failed config copy rejects prepare");
			ok &= expect(runtime_dirs_for_uid(runtime_root, getuid()) == before_config_copy_failure,
			             "failed config copy removes private runtime directory");
			ok &= expect(chmod(config_path.c_str(), 0644) == 0, "restores source config");
		}

		ok &= expect(write_file(model_path, "model-content"), "writes secure source model");
		ok &= expect(chmod(model_path.c_str(), 0644) == 0, "secures source model");
		const auto before_model_success = runtime_dirs_for_uid(runtime_root, getuid());
		const auto prepared_with_model  = prepare_runtime_auth_files_for_test(
		    "alice", getuid(), getgid(), runtime_root, config_path, models_dir, owner_uid);
		ok &= expect(prepared_with_model.has_value(), "secure source model prepares auth files");
		if (prepared_with_model.has_value()) {
			ok &= expect(read_file(prepared_with_model->user_models_dir / "alice.dat") ==
			                 "model-content",
			             "successful prepare stages user model");
			fs::remove_all(prepared_with_model->runtime_dir, ec);
			ok &= expect(!ec, "cleans prepared runtime directory with model");
		}
		ok &= expect(runtime_dirs_for_uid(runtime_root, getuid()) == before_model_success,
		             "successful model prepare fixture leaves no runtime directory");

		ok &= expect(write_file(model_path, "model-content"), "writes insecure source model");
		ok &= expect(chmod(model_path.c_str(), 0664) == 0, "makes source model insecure");
		const auto before_model_failure = runtime_dirs_for_uid(runtime_root, getuid());
		ok &= expect(!prepare_runtime_auth_files_for_test("alice", getuid(), getgid(), runtime_root,
		                                                  config_path, models_dir, owner_uid)
		                  .has_value(),
		             "insecure source model rejects prepare");
		ok &= expect(runtime_dirs_for_uid(runtime_root, getuid()) == before_model_failure,
		             "insecure source model failure removes private runtime directory");

		fs::remove_all(fixture_dir, ec);
		ok &= expect(!ec, "cleans prepare runtime auth files fixtures");
		return ok;
	}

	auto expect_stdout_protocol() -> bool {
		using namespace howdy::native::auth_helper_protocol;
		using howdy::native::testing::print_prepared_paths;

		bool ok = true;
		ok &= expect(std::string(kConfigPathKey) == "CONFIG_PATH",
		             "config path protocol key remains unchanged");
		ok &= expect(std::string(kUserModelsDirKey) == "USER_MODELS_DIR",
		             "user models directory protocol key remains unchanged");

		std::ostringstream output;
		auto              *previous = std::cout.rdbuf(output.rdbuf());
		print_prepared_paths("/run/howdy/pam-1000-example/config.ini",
		                     "/run/howdy/pam-1000-example/models");
		std::cout.rdbuf(previous);

		ok &= expect(output.str() == "CONFIG_PATH=/run/howdy/pam-1000-example/config.ini\n"
		                             "USER_MODELS_DIR=/run/howdy/pam-1000-example/models\n",
		             "prepare stdout protocol remains unchanged");
		return ok;
	}

}  // namespace

auto main() -> int {
	namespace fs = std::filesystem;

	bool       ok = true;
	const auto temp_root =
	    fs::current_path() / ("howdy-auth-helper-test-" + std::to_string(getpid()));
	std::error_code ec;
	fs::remove_all(temp_root, ec);
	fs::create_directories(temp_root, ec);
	ok &= expect(!ec, "creates auth-helper temp root");

	ok &= expect_runtime_root_validation(temp_root);
	ok &= expect_secure_source_file_stat(temp_root);
	ok &= expect_write_all_helper(temp_root);
	ok &= expect_copy_file_for_user(temp_root);
	ok &= expect_source_model_readiness(temp_root);
	ok &= expect_prepare_cleanup_guards();
	ok &= expect_cleanup_runtime_auth_files(temp_root);
	ok &= expect_prepare_runtime_auth_files(temp_root);
	ok &= expect_stdout_protocol();

	fs::remove_all(temp_root, ec);
	return ok ? 0 : 1;
}
