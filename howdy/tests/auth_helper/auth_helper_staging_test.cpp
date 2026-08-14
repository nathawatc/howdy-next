#include "auth_helper/auth_helper_test_groups.hpp"
#include "auth_helper/auth_helper_test_support.hpp"
#include "auth_helper/command.hpp"
#include "auth_helper/runtime_internal.hpp"
#include "protocol/auth_helper_protocol.hpp"

#include <set>
#include <sstream>
#include <tuple>

namespace {
	using namespace howdy::test::auth_helper;

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

	auto expect_prepare_cleanup_guards() -> bool {
		using howdy::native::auth_helper::command::cleanup_for_user;
		using howdy::native::auth_helper::command::prepare_for_user;

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
		using howdy::native::auth_helper::internal::cleanup_runtime_auth_files;

		bool            ok = true;
		std::error_code ec;
		const auto      fixture_root = temp_root / "cleanup-runtime-auth-files";
		const auto      runtime_root = fixture_root / "runtime-root";
		const auto      uid          = getuid();
		const auto      gid          = getgid();
		const howdy::native::auth_helper::RuntimeIdentity identity{.uid = uid, .gid = gid};
		const auto  prefix       = "pam-" + std::to_string(uid) + "-";
		const uid_t non_root_uid = 1;
		const gid_t wrong_gid    = gid == 0 ? static_cast<gid_t>(1) : static_cast<gid_t>(0);

		fs::remove_all(fixture_root, ec);
		fs::create_directories(runtime_root, ec);
		ok &= expect(!ec, "creates cleanup runtime fixtures");
		ok &= expect(chmod(runtime_root.c_str(), 0711) == 0, "secures cleanup runtime root");

		const auto wrong_parent = fixture_root / "wrong-parent" / (prefix + "wrong-parent");
		ok &= expect(!cleanup_runtime_auth_files(wrong_parent, identity.uid, runtime_root).ok,
		             "wrong cleanup parent is rejected");

		const auto wrong_prefix = runtime_root / ("pam-" + std::to_string(uid + 1) + "-wrong");
		ok &= expect(!cleanup_runtime_auth_files(wrong_prefix, identity.uid, runtime_root).ok,
		             "wrong pam uid prefix is rejected");

		const auto missing_runtime_dir = runtime_root / (prefix + "missing");
		ok &= expect(cleanup_runtime_auth_files(missing_runtime_dir, identity.uid, runtime_root).ok,
		             "missing expected runtime dir succeeds");
		ok &=
		    expect(!fs::exists(missing_runtime_dir), "missing expected runtime dir stays missing");

		const auto regular_file = runtime_root / (prefix + "regular");
		ok &= expect(write_file(regular_file, "cleanup"), "writes runtime cleanup file");
		ok &= expect(!cleanup_runtime_auth_files(regular_file, identity.uid, runtime_root).ok,
		             "regular file is rejected for cleanup");
		ok &= expect(fs::exists(regular_file), "regular file remains after rejected cleanup");
		fs::remove(regular_file, ec);
		ec.clear();

		const auto symlink_target = fixture_root / "cleanup-target";
		const auto symlink_path   = runtime_root / (prefix + "symlink");
		ok &= expect(write_file(symlink_target, "target"), "writes cleanup symlink target");
		if (symlink(symlink_target.c_str(), symlink_path.c_str()) == 0) {
			ok &= expect(!cleanup_runtime_auth_files(symlink_path, identity.uid, runtime_root).ok,
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
		const bool      restored_privileged_probe = chown(privileged_probe.c_str(), uid, gid) == 0;
		std::error_code privileged_probe_cleanup_ec;
		fs::remove_all(privileged_probe, privileged_probe_cleanup_ec);
		const bool cleaned_up_privileged_probe =
		    restored_privileged_probe && !privileged_probe_cleanup_ec;
		ec.clear();

		if (!can_setup_privileged_cleanup) {
			std::cerr << "SKIP: cleanup ownership/mode cases need required chown capabilities\n";
		} else {
			const bool privileged_probe_cleanup_ok =
			    expect(cleaned_up_privileged_probe, "cleans up privileged cleanup probe");
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
				    !cleanup_runtime_auth_files(group_writable_dir, identity.uid, runtime_root).ok,
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
				    !cleanup_runtime_auth_files(world_writable_dir, identity.uid, runtime_root).ok,
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
				    cleanup_runtime_auth_files(valid_runtime_dir, identity.uid, runtime_root).ok,
				    "valid runtime directory is removed");
				ok &= expect(!fs::exists(valid_runtime_dir), "valid runtime directory is gone");

				const auto wrong_owner_dir = runtime_root / (prefix + "wrong-owner");
				fs::create_directories(wrong_owner_dir, ec);
				ok &= expect(!ec, "creates wrong-owner cleanup directory");
				ok &= expect(chown(wrong_owner_dir.c_str(), non_root_uid, gid) == 0,
				             "sets wrong-owner cleanup directory");
				ok &= expect(
				    !cleanup_runtime_auth_files(wrong_owner_dir, identity.uid, runtime_root).ok,
				    "wrong owner is rejected");
				fs::remove_all(wrong_owner_dir, ec);
				ec.clear();

				const auto wrong_gid_dir = runtime_root / (prefix + "wrong-gid");
				fs::create_directories(wrong_gid_dir, ec);
				ok &= expect(!ec, "creates wrong-gid cleanup directory");
				ok &= expect(chown(wrong_gid_dir.c_str(), 0, wrong_gid) == 0,
				             "sets wrong-gid cleanup directory");
				ok &= expect(
				    !cleanup_runtime_auth_files(wrong_gid_dir, identity.uid, runtime_root).ok,
				    "wrong gid is rejected");
				fs::remove_all(wrong_gid_dir, ec);
				ec.clear();
			}
		}

		fs::remove_all(fixture_root, ec);
		ok &= expect(!ec, "cleans cleanup runtime fixtures");
		return ok;
	}

	auto expect_prepare_runtime_auth_files(
	    const std::filesystem::path &temp_root, bool acl_functional, uid_t target_uid,
	    const howdy::native::auth_helper::AclOperations &operations) -> bool {
		namespace fs = std::filesystem;
		using howdy::native::auth_helper::internal::prepare_runtime_auth_files;

		bool            ok = true;
		std::error_code ec;
		const auto      fixture_dir  = temp_root / "prepare-runtime-auth-files";
		const auto      runtime_root = fixture_dir / "runtime-root";
		const auto      source_dir   = fixture_dir / "source";
		const auto      original_dir = fs::current_path();
		const auto      config_path  = fs::path("./config.ini");
		const auto      models_dir   = fs::path("./models");
		const auto      model_path   = models_dir / "alice.dat";
		const uid_t     owner_uid    = geteuid();
		const howdy::native::auth_helper::internal::StagedIdentity identity{
		    .target_uid = target_uid, .owner_uid = owner_uid, .owner_gid = getegid()};
		const howdy::native::auth_helper::internal::RuntimeSources source_paths{
		    .runtime_root    = runtime_root,
		    .config          = config_path,
		    .user_models_dir = models_dir,
		};
		fs::remove_all(fixture_dir, ec);
		fs::create_directories(runtime_root, ec);
		ok &= expect(!ec, "creates injected runtime root");
		ok &= expect(chmod(runtime_root.c_str(), 0711) == 0, "secures injected runtime root");
		fs::create_directories(source_dir / "models", ec);
		ok &= expect(!ec, "creates prepare runtime auth files fixtures");
		fs::current_path(source_dir, ec);
		ok &= expect(!ec, "uses relative secure source paths");
		ok &= expect(write_file(config_path, "config-content"), "writes secure source config");
		ok &= expect(chmod(fixture_dir.c_str(), 0755) == 0, "secures source fixture directory");
		ok &= expect(chmod(source_dir.c_str(), 0755) == 0, "secures source directory");
		ok &= expect(chmod(config_path.c_str(), 0644) == 0, "secures source config");
		ok &= expect(chmod(models_dir.c_str(), 0755) == 0, "secures source models directory");

		const auto invalid_runtime_root = fixture_dir / "runtime-root-file";
		ok &= expect(write_file(invalid_runtime_root, "not a directory"),
		             "writes invalid runtime root fixture");
		const auto before_invalid_root = runtime_dirs_for_uid(fixture_dir, target_uid);
		const auto invalid_root_prepare =
		    prepare_runtime_auth_files("alice", identity,
		                               {.runtime_root    = invalid_runtime_root,
		                                .config          = config_path,
		                                .user_models_dir = models_dir},
		                               operations);
		ok &= expect(!invalid_root_prepare.has_value(),
		             "regular runtime root rejects higher-level preparation");
		ok &= expect(runtime_dirs_for_uid(fixture_dir, target_uid) == before_invalid_root,
		             "invalid runtime root leaves no partial runtime directory");

		if (acl_functional) {
			const auto before_success = runtime_dirs_for_uid(runtime_root, target_uid);
			const auto prepared =
			    prepare_runtime_auth_files("alice", identity, source_paths, operations);
			ok &=
			    expect(prepared.has_value(), "missing source user model still prepares auth files");
			if (prepared.has_value()) {
				const auto prefix = "pam-" + std::to_string(target_uid) + "-";
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
				ok &= expect(chmod(prepared->user_models_dir.c_str(), 0700) == 0,
				             "reopens successful prepared models directory for cleanup");
				ok &= expect(chmod(prepared->runtime_dir.c_str(), 0700) == 0,
				             "reopens successful prepared runtime directory for cleanup");
				fs::remove_all(prepared->runtime_dir, ec);
				ok &= expect(!ec, "cleans successful prepared runtime directory");
			}
			ok &= expect(runtime_dirs_for_uid(runtime_root, target_uid) == before_success,
			             "successful prepare fixture leaves no runtime directory");
		}

		const auto before_config_failure = runtime_dirs_for_uid(runtime_root, target_uid);
		ok &= expect(!prepare_runtime_auth_files("alice", identity,
		                                         {.runtime_root    = runtime_root,
		                                          .config          = source_dir / "missing.ini",
		                                          .user_models_dir = models_dir},
		                                         operations)
		                  .has_value(),
		             "failed config staging rejects prepare");
		ok &= expect(runtime_dirs_for_uid(runtime_root, target_uid) == before_config_failure,
		             "failed config staging removes private runtime directory");

		if (geteuid() == 0) {
			std::cerr << "SKIP: config copy permission failure requires non-root test process\n";
		} else {
			ok &= expect(chmod(config_path.c_str(), 0000) == 0, "makes source config unreadable");
			const auto before_config_copy_failure = runtime_dirs_for_uid(runtime_root, target_uid);
			ok &= expect(!prepare_runtime_auth_files("alice", identity, source_paths, operations)
			                  .has_value(),
			             "failed config copy rejects prepare");
			ok &=
			    expect(runtime_dirs_for_uid(runtime_root, target_uid) == before_config_copy_failure,
			           "failed config copy removes private runtime directory");
			ok &= expect(chmod(config_path.c_str(), 0644) == 0, "restores source config");
		}

		if (acl_functional) {
			ok &= expect(write_file(model_path, "model-content"), "writes secure source model");
			ok &= expect(chmod(model_path.c_str(), 0644) == 0, "secures source model");
			const auto before_model_success = runtime_dirs_for_uid(runtime_root, target_uid);
			const auto prepared_with_model =
			    prepare_runtime_auth_files("alice", identity, source_paths, operations);
			ok &=
			    expect(prepared_with_model.has_value(), "secure source model prepares auth files");
			if (prepared_with_model.has_value()) {
				ok &= expect(read_file(prepared_with_model->user_models_dir / "alice.dat") ==
				                 "model-content",
				             "successful prepare stages user model");
				ok &= expect(chmod(prepared_with_model->user_models_dir.c_str(), 0700) == 0,
				             "reopens prepared models directory with model for cleanup");
				ok &= expect(chmod(prepared_with_model->runtime_dir.c_str(), 0700) == 0,
				             "reopens prepared runtime directory with model for cleanup");
				fs::remove_all(prepared_with_model->runtime_dir, ec);
				ok &= expect(!ec, "cleans prepared runtime directory with model");
			}
			ok &= expect(runtime_dirs_for_uid(runtime_root, target_uid) == before_model_success,
			             "successful model prepare fixture leaves no runtime directory");

			ok &= expect(write_file(model_path, "model-content"), "writes insecure source model");
			ok &= expect(chmod(model_path.c_str(), 0664) == 0, "makes source model insecure");
			const auto before_model_failure = runtime_dirs_for_uid(runtime_root, target_uid);
			ok &= expect(!prepare_runtime_auth_files("alice", identity, source_paths, operations)
			                  .has_value(),
			             "insecure source model rejects prepare");
			ok &= expect(runtime_dirs_for_uid(runtime_root, target_uid) == before_model_failure,
			             "insecure source model failure removes private runtime directory");
		}

		fs::current_path(original_dir, ec);
		ok &= expect(!ec, "restores test working directory");
		fs::remove_all(fixture_dir, ec);
		ok &= expect(!ec, "cleans prepare runtime auth files fixtures");
		return ok;
	}

	auto
	expect_wrong_owner_config_rejected(const std::filesystem::path                     &temp_root,
	                                   const howdy::native::auth_helper::AclOperations &operations)
	    -> bool {
		namespace fs = std::filesystem;
		using howdy::native::auth_helper::internal::prepare_runtime_auth_files;

		if (geteuid() != 0) {
			std::cerr << "SKIP: wrong-owner config fixture requires chown capability\n";
			return true;
		}

		bool            ok = true;
		std::error_code ec;
		const auto      fixture_dir  = temp_root / "wrong-owner-config";
		const auto      runtime_root = fixture_dir / "runtime-root";
		const auto      source_dir   = fixture_dir / "source";
		const auto      config_path  = source_dir / "config.ini";
		const auto      models_dir   = source_dir / "models";
		fs::create_directories(runtime_root, ec);
		ok &= expect(!ec, "creates wrong-owner runtime root");
		ec.clear();
		fs::create_directories(models_dir, ec);
		ok &= expect(!ec, "creates wrong-owner models directory");
		ok &= expect(write_file(config_path, "config-content"), "writes wrong-owner source config");
		ok &=
		    expect(chmod(fixture_dir.c_str(), 0755) == 0, "secures wrong-owner fixture directory");
		ok &= expect(chmod(source_dir.c_str(), 0755) == 0, "secures wrong-owner source directory");
		ok &= expect(chmod(models_dir.c_str(), 0755) == 0, "secures wrong-owner models directory");
		ok &= expect(chmod(config_path.c_str(), 0644) == 0, "secures wrong-owner source config");
		if (chown(config_path.c_str(), 61006, 0) != 0) {
			std::cerr << "SKIP: wrong-owner config fixture chown failed: " << std::strerror(errno)
			          << "\n";
			fs::remove_all(fixture_dir, ec);
			return ok;
		}
		const auto before_prepare = runtime_dirs_for_uid(runtime_root, getuid());
		const auto prepared       = prepare_runtime_auth_files(
		    "alice", {.target_uid = getuid(), .owner_uid = 0, .owner_gid = getegid()},
		    {.runtime_root = runtime_root, .config = config_path, .user_models_dir = models_dir},
		    operations);
		ok &= expect(!prepared.has_value(), "wrong-owner config rejects runtime preparation");
		ok &= expect(runtime_dirs_for_uid(runtime_root, getuid()) == before_prepare,
		             "wrong-owner config removes partial runtime tree");
		fs::remove_all(fixture_dir, ec);
		ok &= expect(!ec, "cleans wrong-owner config fixtures");
		return ok;
	}

	auto expect_acl_setup_failure(const std::filesystem::path &temp_root) -> bool {
		namespace fs = std::filesystem;
		using howdy::native::auth_helper::internal::prepare_runtime_auth_files;

		bool            ok = true;
		std::error_code ec;
		const auto      fixture_dir  = temp_root / "acl-setup-failure";
		const auto      runtime_root = fixture_dir / "runtime-root";
		const auto      source_dir   = fixture_dir / "source";
		const auto      original_dir = fs::current_path();
		const auto      config_path  = fs::path("./config.ini");
		const auto      models_dir   = fs::path("./models");
		const uid_t     target_uid   = geteuid() == 0 ? 61001 : geteuid();

		fs::create_directories(runtime_root, ec);
		ok &= expect(!ec, "creates ACL failure runtime root");
		ec.clear();
		fs::create_directories(source_dir / "models", ec);
		ok &= expect(!ec, "creates ACL failure source models directory");
		fs::current_path(source_dir, ec);
		ok &= expect(!ec, "uses relative ACL failure source paths");
		ok &= expect(write_file(config_path, "config-content"), "writes ACL failure source config");
		ok &= expect(write_file(models_dir / "alice.dat", "model-content"),
		             "writes ACL failure source model");
		ok &=
		    expect(chmod(fixture_dir.c_str(), 0755) == 0, "secures ACL failure fixture directory");
		ok &= expect(chmod(source_dir.c_str(), 0755) == 0, "secures ACL failure source directory");
		ok &= expect(chmod(config_path.c_str(), 0644) == 0, "secures ACL failure source config");
		ok &= expect(chmod(models_dir.c_str(), 0755) == 0,
		             "secures ACL failure source models directory");
		ok &= expect(chmod((models_dir / "alice.dat").c_str(), 0644) == 0,
		             "secures ACL failure source model");

		FakeAclContext state;
		state.set_failure                 = true;
		const auto         before_failure = runtime_dirs_for_uid(runtime_root, target_uid);
		std::ostringstream failure_output;
		auto              *previous_cerr  = std::cerr.rdbuf(failure_output.rdbuf());
		const auto         failed_prepare = prepare_runtime_auth_files(
		    "alice", {.target_uid = target_uid, .owner_uid = geteuid(), .owner_gid = getegid()},
		    {.runtime_root = runtime_root, .config = config_path, .user_models_dir = models_dir},
		    state.operations());
		std::cerr.rdbuf(previous_cerr);
		ok &= expect(!failed_prepare.has_value(), "ACL setup failure rejects preparation");
		ok &= expect(failure_output.str().contains("acl_set_fd for staged object"),
		             "ACL setup failure logs operation");
		ok &= expect(failure_output.str().contains(runtime_root.string()),
		             "ACL setup failure logs staged path");
		ok &= expect(failure_output.str().contains(std::strerror(EIO)),
		             "ACL setup failure logs saved errno");
		ok &= expect(runtime_dirs_for_uid(runtime_root, target_uid) == before_failure,
		             "ACL setup failure removes partial runtime directory");

		state.clear();
		state.verification_failure               = true;
		const auto         before_verify_failure = runtime_dirs_for_uid(runtime_root, target_uid);
		std::ostringstream verify_failure_output;
		previous_cerr                    = std::cerr.rdbuf(verify_failure_output.rdbuf());
		const auto verify_failed_prepare = prepare_runtime_auth_files(
		    "alice", {.target_uid = target_uid, .owner_uid = geteuid(), .owner_gid = getegid()},
		    {.runtime_root = runtime_root, .config = config_path, .user_models_dir = models_dir},
		    state.operations());
		std::cerr.rdbuf(previous_cerr);
		ok &= expect(!verify_failed_prepare.has_value(), "ACL policy mismatch rejects preparation");
		ok &= expect(verify_failure_output.str().contains("ACL policy mismatch"),
		             "ACL policy mismatch logs policy diagnostic");
		ok &= expect(verify_failure_output.str().contains(runtime_root.string()),
		             "ACL policy mismatch logs staged path");
		ok &= expect(!verify_failure_output.str().contains("Success"),
		             "ACL policy mismatch does not log misleading Success errno");
		ok &= expect(runtime_dirs_for_uid(runtime_root, target_uid) == before_verify_failure,
		             "ACL policy mismatch removes partial runtime directory");

		fs::current_path(original_dir, ec);
		ok &= expect(!ec, "restores ACL failure test working directory");
		fs::remove_all(fixture_dir, ec);
		ok &= expect(!ec, "cleans ACL failure fixtures");
		return ok;
	}

	auto
	expect_staged_acl_confidentiality(const std::filesystem::path                     &temp_root,
	                                  const howdy::native::auth_helper::AclOperations &operations)
	    -> bool {
		namespace fs = std::filesystem;
		using howdy::native::auth_helper::internal::prepare_runtime_auth_files;

		bool            ok = true;
		std::error_code ec;
		const auto      fixture_dir   = temp_root / "staged-acl-confidentiality";
		const auto      runtime_root  = fixture_dir / "runtime-root";
		const auto      source_dir    = fixture_dir / "source";
		const auto      original_dir  = fs::current_path();
		const auto      config_path   = fs::path("./config.ini");
		const auto      models_dir    = fs::path("./models");
		const uid_t     target_uid    = geteuid() == 0 ? 61001 : geteuid();
		const uid_t     shared_uid    = 61002;
		const uid_t     unrelated_uid = 61003;
		const gid_t     shared_gid    = 61004;
		const gid_t     unrelated_gid = 61005;
		const uid_t     owner_uid     = geteuid();
		const gid_t     owner_gid     = getegid();

		fs::create_directories(runtime_root, ec);
		ok &= expect(!ec, "creates ACL runtime root");
		ec.clear();
		fs::create_directories(source_dir / "models", ec);
		ok &= expect(!ec, "creates ACL source models directory");
		fs::current_path(source_dir, ec);
		ok &= expect(!ec, "uses relative ACL source paths");
		ok &= expect(write_file(config_path, "config-content"), "writes ACL source config");
		ok &= expect(write_file(models_dir / "alice.dat", "model-content"),
		             "writes ACL source model");
		ok &= expect(chmod(fixture_dir.c_str(), 0755) == 0, "secures ACL fixture directory");
		ok &= expect(chmod(source_dir.c_str(), 0755) == 0, "secures ACL source directory");
		ok &= expect(chmod(config_path.c_str(), 0644) == 0, "secures ACL source config");
		ok &= expect(chmod(models_dir.c_str(), 0755) == 0, "secures ACL source models directory");
		ok &= expect(chmod((models_dir / "alice.dat").c_str(), 0644) == 0,
		             "secures ACL source model");

		const auto before_prepare = runtime_dirs_for_uid(runtime_root, target_uid);
		const auto prepared       = prepare_runtime_auth_files(
		    "alice", {.target_uid = target_uid, .owner_uid = owner_uid, .owner_gid = owner_gid},
		    {.runtime_root = runtime_root, .config = config_path, .user_models_dir = models_dir},
		    operations);
		ok &= expect(prepared.has_value(), "ACL runtime preparation succeeds");
		if (prepared.has_value()) {
			for (const auto &[path, directory, label] : {
			         std::tuple{prepared->runtime_dir, true, "runtime directory"},
			         std::tuple{prepared->user_models_dir, true, "models directory"},
			         std::tuple{prepared->config_path, false, "config file"},
			         std::tuple{prepared->user_models_dir / "alice.dat", false, "model file"},
			     }) {
				struct stat stat_{};
				ok &= expect(lstat(path.c_str(), &stat_) == 0, std::string("stats ") + label);
				ok &= expect(stat_.st_uid == owner_uid && stat_.st_gid == owner_gid,
				             std::string(label) + " remains expected privileged owner");
				ok &= expect_private_acl(path, target_uid, directory, label);
			}
			if (geteuid() == 0) {
				ok &= expect(child_can_access_staged_files(*prepared, target_uid, shared_gid, true),
				             "named target UID can traverse and read staged files");
				ok &=
				    expect(child_can_access_staged_files(*prepared, shared_uid, shared_gid, false),
				           "same primary GID UID cannot traverse or read staged files");
				ok &= expect(
				    child_can_access_staged_files(*prepared, unrelated_uid, unrelated_gid, false),
				    "unrelated UID cannot traverse or read staged files");
			} else {
				std::cerr << "SKIP: cross-UID access checks require root\n";
			}
			ok &= expect(chmod(prepared->user_models_dir.c_str(), 0700) == 0,
			             "reopens ACL prepared models directory for cleanup");
			ok &= expect(chmod(prepared->runtime_dir.c_str(), 0700) == 0,
			             "reopens ACL prepared runtime directory for cleanup");
			fs::remove_all(prepared->runtime_dir, ec);
			ok &= expect(!ec, "cleans ACL prepared runtime directory");
		}
		ok &= expect(runtime_dirs_for_uid(runtime_root, target_uid) == before_prepare,
		             "ACL prepared runtime directory is removed");

		fs::current_path(original_dir, ec);
		ok &= expect(!ec, "restores ACL test working directory");
		fs::remove_all(fixture_dir, ec);
		ok &= expect(!ec, "cleans ACL staging fixtures");
		return ok;
	}

	auto expect_stdout_protocol() -> bool {
		using namespace howdy::native::auth_helper_protocol;
		using howdy::native::auth_helper::command::print_prepared_paths;

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

auto run_auth_helper_staging_tests(const std::filesystem::path    &temp_root,
                                   const AuthHelperStagingContext &context) -> bool {
	bool ok = true;
	if (context.acl_supported && geteuid() == 0 && getuid() == geteuid()) {
		constexpr uid_t kDistinctTargetUid = 61001;
		ok &= expect_prepare_runtime_auth_files(temp_root, true, kDistinctTargetUid,
		                                        context.fake_acl.operations());
		ok &= expect_fake_acl_activity(context.fake_acl, "distinct-target fake ACL preparation");
		ok &= expect_fake_acl_target(context.fake_acl, kDistinctTargetUid, geteuid());
		ok &= reset_fake_acl_backend(context.fake_acl);
	}
	ok &= expect_prepare_cleanup_guards();
	ok &= expect_cleanup_runtime_auth_files(temp_root);
	ok &= expect_wrong_owner_config_rejected(temp_root, context.operations);
	ok &= expect_prepare_runtime_auth_files(temp_root, context.acl_functional,
	                                        context.functional_target_uid, context.operations);
	if (context.use_fake_acl) {
		ok &= expect_fake_acl_activity(context.fake_acl, "prepare_runtime_auth_files");
		ok &= expect_fake_acl_target(context.fake_acl, context.functional_target_uid, geteuid());
		ok &= reset_fake_acl_backend(context.fake_acl);
	}
	ok &= expect_acl_setup_failure(temp_root);
	if (context.acl_supported) {
		ok &= expect_staged_acl_confidentiality(temp_root, context.production_operations);
	}
	ok &= expect_stdout_protocol();
	return ok;
}
