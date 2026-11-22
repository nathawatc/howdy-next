#pragma once

#include "storage/user_model_store.hpp"
#include "storage/user_model_types.hpp"
#include "test_support.hpp"

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

#include <fcntl.h>
#include <unistd.h>

#include <sys/stat.h>

namespace howdy::test::user_models {

	using howdy::test::Expect;
	using howdy::test::NestedArray;
	using howdy::test::ReadFile;
	using howdy::test::WriteFile;

	inline auto ExpectationFromEntry(const howdy::native::UserModelEntry &entry)
	    -> howdy::native::UserModelEntryExpectation {
		return howdy::native::UserModelEntryExpectation{
		    .id      = entry.id,
		    .time    = entry.time,
		    .label   = entry.label,
		    .backend = entry.backend,
		    .metric  = entry.metric,
		    .model   = entry.model,
		};
	}

	struct PostLockSymlinkSwap {
		std::filesystem::path victim_path;
		int                   calls   = 0;
		bool                  swapped = false;
	};

	struct PostLockRegularFileSwap {
		std::filesystem::path replacement_path;
		int                   calls   = 0;
		bool                  swapped = false;
	};

	struct AbaRegularFileSwap {
		std::filesystem::path replacement_path;
		std::filesystem::path original_saved_path;
		std::filesystem::path locked_replacement_saved_path;
		int                   before_lock_calls = 0;
		int                   after_lock_calls  = 0;
		bool                  installed_b       = false;
		bool                  restored_a        = false;
		bool                  rollback_failed   = false;
	};

	struct PreservingRegularFileSwap {
		std::filesystem::path replacement_path;
		std::filesystem::path displaced_path;
		std::filesystem::path target_path;
		int                   calls           = 0;
		bool                  swapped         = false;
		bool                  rollback_failed = false;
	};

	inline auto ReplaceWithVictimSymlinkAfterLock(PostLockSymlinkSwap         *swap,
	                                              const std::filesystem::path &path) -> void {
		++swap->calls;
		const auto      symlink_path = path.string() + ".post-lock-symlink";
		std::error_code ec;
		std::filesystem::remove(symlink_path, ec);
		ec.clear();
		if (symlink(swap->victim_path.c_str(), symlink_path.c_str()) != 0) {
			return;
		}
		if (rename(symlink_path.c_str(), path.c_str()) != 0) {
			std::filesystem::remove(symlink_path, ec);
			return;
		}
		swap->swapped = true;
	}

	inline auto ReplaceWithRegularFileAfterLock(PostLockRegularFileSwap     *swap,
	                                            const std::filesystem::path &path) -> void {
		++swap->calls;
		if (rename(swap->replacement_path.c_str(), path.c_str()) != 0) {
			return;
		}
		swap->swapped = true;
	}

	inline auto InstallReplacementBeforeLock(AbaRegularFileSwap          *swap,
	                                         const std::filesystem::path &path) -> void {
		++swap->before_lock_calls;
		if (rename(path.c_str(), swap->original_saved_path.c_str()) != 0) {
			return;
		}
		if (rename(swap->replacement_path.c_str(), path.c_str()) != 0) {
			swap->rollback_failed = rename(swap->original_saved_path.c_str(), path.c_str()) != 0;
			return;
		}
		swap->installed_b = true;
	}

	inline auto RestoreOriginalAfterLock(AbaRegularFileSwap          *swap,
	                                     const std::filesystem::path &path) -> void {
		++swap->after_lock_calls;
		if (rename(path.c_str(), swap->locked_replacement_saved_path.c_str()) != 0) {
			return;
		}
		if (rename(swap->original_saved_path.c_str(), path.c_str()) != 0) {
			swap->rollback_failed =
			    rename(swap->locked_replacement_saved_path.c_str(), path.c_str()) != 0;
			return;
		}
		swap->restored_a = true;
	}

	inline auto ReplacePathPreservingOriginal(PreservingRegularFileSwap   *swap,
	                                          const std::filesystem::path &path) -> void {
		++swap->calls;
		swap->target_path = path;
		if (rename(path.c_str(), swap->displaced_path.c_str()) != 0) {
			return;
		}
		if (rename(swap->replacement_path.c_str(), path.c_str()) != 0) {
			swap->rollback_failed = rename(swap->displaced_path.c_str(), path.c_str()) != 0;
			return;
		}
		swap->swapped = true;
	}

	inline auto ExpectReadinessChecks(const std::filesystem::path &temp_root) -> bool {
		namespace fs = std::filesystem;
		using howdy::native::UserModelDescriptorStatus;
		using howdy::native::UserModelStatus;

		bool            ok = true;
		std::error_code ec;
		const auto      models_dir = temp_root / "explicit-readiness-models";
		const auto      model_path = models_dir / "readiness-user.dat";
		const auto      readiness_store =
		    [&](const fs::path &dir, uid_t owner_uid = geteuid()) -> howdy::native::UserModelStore {
			return howdy::native::UserModelStore({
			    .models_dir      = dir,
			    .owner_uid       = owner_uid,
			    .validation_root = {.path = temp_root},
			});
		};
		const auto store           = readiness_store(models_dir);
		const auto expect_insecure = [&](const howdy::native::UserModelStore &target_store,
		                                 const std::string                   &description) -> void {
			ok &= Expect(target_store.InspectFile("readiness-user").status ==
			                 UserModelStatus::kInsecurePath,
			             description + " (inspect)");
			ok &= Expect(target_store.OpenRuntimeDescriptor("readiness-user").status ==
			                 UserModelDescriptorStatus::kInvalidStorage,
			             description + " (open descriptor)");
		};

		fs::remove_all(models_dir, ec);
		ec.clear();
		{
			const auto inspected = store.InspectFile("readiness-user");
			ok &= Expect(inspected.status == UserModelStatus::kNoModelDirectory,
			             "readiness missing model directory returns kNoModelDirectory");
			const auto listed = store.ListEntries("readiness-user", {});
			ok &= Expect(listed.status == UserModelStatus::kNoModelDirectory,
			             "list missing model directory returns kNoModelDirectory");
			const auto opened = store.OpenRuntimeDescriptor("readiness-user");
			ok &= Expect(opened.status == UserModelDescriptorStatus::kMissing && !opened.fd.Valid(),
			             "open descriptor missing model directory returns kMissing");
		}
		{
			const auto inspected = store.InspectFile("../alice");
			ok &= Expect(inspected.status == UserModelStatus::kInvalidUser,
			             "readiness invalid username returns kInvalidUser");
			const auto opened = store.OpenRuntimeDescriptor("../alice");
			ok &= Expect(opened.status == UserModelDescriptorStatus::kInvalidUser &&
			                 !opened.fd.Valid(),
			             "open descriptor invalid username returns kInvalidUser");
		}

		const auto real_models_dir    = temp_root / "real-readiness-models";
		const auto symlink_models_dir = temp_root / "symlink-readiness-models";
		fs::remove_all(real_models_dir, ec);
		fs::remove(symlink_models_dir, ec);
		ec.clear();
		fs::create_directories(real_models_dir, ec);
		ok &= Expect(!ec, "create real readiness models directory");
		ok &= Expect(chmod(real_models_dir.c_str(), 0755) == 0,
		             "set real readiness models directory mode");
		if (symlink(real_models_dir.c_str(), symlink_models_dir.c_str()) == 0) {
			expect_insecure(readiness_store(symlink_models_dir),
			                "readiness rejects symlinked models directory");
			fs::remove(symlink_models_dir, ec);
			ec.clear();
		} else {
			std::cerr << "SKIP: readiness models directory symlink creation failed\n";
		}
		fs::remove_all(real_models_dir, ec);
		ec.clear();

		const auto dangling_models_dir   = temp_root / "dangling-readiness-models";
		const auto missing_models_target = temp_root / "missing-readiness-target";
		fs::remove(dangling_models_dir, ec);
		ec.clear();
		if (symlink(missing_models_target.c_str(), dangling_models_dir.c_str()) == 0) {
			expect_insecure(readiness_store(dangling_models_dir),
			                "readiness rejects dangling symlinked models directory");
			fs::remove(dangling_models_dir, ec);
			ec.clear();
		} else {
			std::cerr << "SKIP: dangling readiness models directory symlink creation failed\n";
		}

		fs::create_directories(models_dir, ec);
		ok &= Expect(!ec, "create explicit readiness models directory");
		ok &= Expect(chmod(models_dir.c_str(), 0755) == 0,
		             "set explicit readiness models directory mode");
		{
			const auto inspected = store.InspectFile("readiness-user");
			ok &= Expect(inspected.status == UserModelStatus::kNoModel,
			             "readiness missing model file returns kNoModel");
			const auto opened = store.OpenRuntimeDescriptor("readiness-user");
			ok &= Expect(opened.status == UserModelDescriptorStatus::kMissing && !opened.fd.Valid(),
			             "open descriptor missing model file returns kMissing");
		}

		ok &= Expect(WriteFile(model_path, "not-json"), "write readiness model file");
		ok &= Expect(chmod(model_path.c_str(), 0644) == 0, "set readiness model file mode");
		{
			const auto inspected = store.InspectFile("readiness-user");
			ok &= Expect(inspected.status == UserModelStatus::kOk,
			             "readiness accepts secure model without parsing JSON");
			const auto  opened = store.OpenRuntimeDescriptor("readiness-user");
			struct stat opened_stat{};
			struct stat expected_stat{};
			ok &= Expect(opened.status == UserModelDescriptorStatus::kReady && opened.fd.Valid() &&
			                 fstat(opened.fd.Get(), &opened_stat) == 0 &&
			                 stat(model_path.c_str(), &expected_stat) == 0 &&
			                 opened_stat.st_dev == expected_stat.st_dev &&
			                 opened_stat.st_ino == expected_stat.st_ino,
			             "open descriptor respects explicit models directory without parsing JSON");
			expect_insecure(readiness_store(models_dir, static_cast<uid_t>(geteuid() + 1)),
			                "readiness rejects mismatched owner");
		}

		const auto symlink_target = models_dir / "readiness-target.dat";
		ok &= Expect(WriteFile(symlink_target, "not-json"), "write readiness symlink target");
		fs::remove(model_path, ec);
		ec.clear();
		if (symlink(symlink_target.c_str(), model_path.c_str()) == 0) {
			expect_insecure(store, "readiness rejects symlinked model file");
			ok &= Expect(fs::remove(model_path, ec), "remove readiness model symlink");
			ec.clear();
		} else {
			std::cerr << "SKIP: readiness model symlink creation failed\n";
		}

		const auto missing_target = models_dir / "readiness-missing-target.dat";
		if (symlink(missing_target.c_str(), model_path.c_str()) == 0) {
			expect_insecure(store, "readiness rejects dangling model symlink");
			ok &= Expect(fs::remove(model_path, ec), "remove dangling readiness model symlink");
			ec.clear();
		} else {
			std::cerr << "SKIP: dangling readiness model symlink creation failed\n";
		}

		ok &= Expect(WriteFile(model_path, "not-json"), "restore readiness model after symlink");
		const auto hardlink_path = models_dir / "readiness-hardlink.dat";
		if (link(model_path.c_str(), hardlink_path.c_str()) == 0) {
			expect_insecure(store, "readiness rejects hard-linked model file");
			ok &= Expect(fs::remove(hardlink_path, ec), "remove readiness hardlink");
			ec.clear();
		} else {
			std::cerr << "SKIP: readiness model hardlink creation failed\n";
		}

		ok &= Expect(chmod(model_path.c_str(), 0664) == 0, "make readiness model group-writable");
		expect_insecure(store, "readiness rejects group-writable model file");
		ok &= Expect(chmod(model_path.c_str(), 0666) == 0, "make readiness model world-writable");
		expect_insecure(store, "readiness rejects world-writable model file");
		ok &= Expect(chmod(model_path.c_str(), 0644) == 0, "restore readiness model file mode");

		ok &= Expect(chmod(models_dir.c_str(), 0775) == 0,
		             "make readiness models directory group-writable");
		expect_insecure(store, "readiness rejects group-writable models directory");
		ok &= Expect(chmod(models_dir.c_str(), 0777) == 0,
		             "make readiness models directory world-writable");
		expect_insecure(store, "readiness rejects world-writable models directory");
		ok &=
		    Expect(chmod(models_dir.c_str(), 0755) == 0, "restore readiness models directory mode");

		fs::remove(model_path, ec);
		ec.clear();
		fs::create_directory(model_path, ec);
		ok &= Expect(!ec, "create non-regular readiness model path");
		expect_insecure(store, "readiness rejects non-regular model file");

		fs::remove_all(models_dir, ec);
		return ok;
	}

	template <typename Value, typename Callback>
	void WithPresent(const std::optional<Value> &value, Callback callback) {
		if (value.has_value()) {
			callback();
		}
	}

	template <typename Callback>
	void WhenSupported(bool supported, Callback callback, std::string_view skip_message) {
		if (supported) {
			callback();
		} else {
			std::cerr << "SKIP: " << skip_message << "\n";
		}
	}

	inline auto StagedUserModelPaths(const std::filesystem::path &models_dir)
	    -> std::vector<std::filesystem::path> {
		std::vector<std::filesystem::path> paths;
		for (const auto &entry : std::filesystem::directory_iterator(models_dir)) {
			if (entry.path().filename().string().starts_with(".howdy-user-model-")) {
				paths.push_back(entry.path());
			}
		}
		return paths;
	}

	inline auto MakeTestStoreConfig(const std::filesystem::path &models_dir,
	                                const std::filesystem::path &validation_root,
	                                uid_t                        owner_uid = geteuid(),
	                                howdy::native::user_model_store_test_hooks::Hooks hooks = {})
	    -> howdy::native::UserModelStoreConfig {
		return {
		    .models_dir      = models_dir,
		    .owner_uid       = owner_uid,
		    .validation_root = {.path = validation_root},
		    .hooks           = std::move(hooks),
		};
	}

	inline auto MakeTestStore(const std::filesystem::path &models_dir,
	                          const std::filesystem::path &validation_root,
	                          uid_t                        owner_uid                  = geteuid(),
	                          howdy::native::user_model_store_test_hooks::Hooks hooks = {})
	    -> howdy::native::UserModelStore {
		return howdy::native::UserModelStore(
		    MakeTestStoreConfig(models_dir, validation_root, owner_uid, std::move(hooks)));
	}

	inline auto WithTestHooks(howdy::native::UserModelStoreConfig               config,
	                          howdy::native::user_model_store_test_hooks::Hooks hooks)
	    -> howdy::native::UserModelStore {
		config.hooks = std::move(hooks);
		return howdy::native::UserModelStore(std::move(config));
	}

	inline auto RequireSnapshot(const howdy::native::UserModelStore &store, const std::string &user)
	    -> std::optional<howdy::native::UserModelFileSnapshot> {
		const auto inspection = store.InspectFile(user);
		if (inspection.status != howdy::native::UserModelStatus::kOk ||
		    !inspection.snapshot.has_value()) {
			return std::nullopt;
		}
		return inspection.snapshot;
	}

	auto TestUserModelStatusClassification() -> bool;
	auto TestUserModelLoading() -> bool;
	auto TestUserModelDescriptorLoading() -> bool;
	auto TestUserModelMutationStart() -> bool;
	auto TestUserModelMutationFailures() -> bool;
	auto TestUserModelVerifiedOperations() -> bool;

}  // namespace howdy::test::user_models
