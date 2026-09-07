#pragma once
#include "storage/user_model_readiness.hpp"
#include "storage/user_models.hpp"
#include "test_support.hpp"

#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>

#include <sys/stat.h>

namespace howdy::test::user_models {

	using howdy::test::expect;
	using howdy::test::read_file;
	using howdy::test::write_file;

	inline auto NestedArray(std::size_t depth) -> std::string {
		return std::string(depth, '[') + "0" + std::string(depth, ']');
	}

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
	};

	struct PreservingRegularFileSwap {
		std::filesystem::path replacement_path;
		std::filesystem::path displaced_path;
		std::filesystem::path target_path;
		int                   calls   = 0;
		bool                  swapped = false;
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
			rename(swap->original_saved_path.c_str(), path.c_str());
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
			rename(swap->locked_replacement_saved_path.c_str(), path.c_str());
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
			rename(swap->displaced_path.c_str(), path.c_str());
			return;
		}
		swap->swapped = true;
	}

	inline auto ExpectReadinessChecks(const std::filesystem::path &temp_root) -> bool {
		namespace fs = std::filesystem;
		using howdy::native::UserModelStatus;

		bool            ok = true;
		std::error_code ec;
		const auto      models_dir = temp_root / "explicit-readiness-models";
		const auto      model_path = models_dir / "readiness-user.dat";

		fs::remove_all(models_dir, ec);
		ec.clear();
		{
			const auto result =
			    howdy::native::CheckUserModelReadiness(models_dir, "readiness-user", std::nullopt);
			ok &= expect(result.status == UserModelStatus::kNoModelDirectory,
			             "readiness missing model directory returns kNoModelDirectory");
		}
		{
			const auto result =
			    howdy::native::CheckUserModelReadiness(models_dir, "../alice", std::nullopt);
			ok &= expect(result.status == UserModelStatus::kInvalidUser,
			             "readiness invalid username returns kInvalidUser");
		}

		const auto real_models_dir    = temp_root / "real-readiness-models";
		const auto symlink_models_dir = temp_root / "symlink-readiness-models";
		fs::remove_all(real_models_dir, ec);
		fs::remove(symlink_models_dir, ec);
		ec.clear();
		fs::create_directories(real_models_dir, ec);
		ok &= expect(!ec, "create real readiness models directory");
		ok &= expect(chmod(real_models_dir.c_str(), 0755) == 0,
		             "set real readiness models directory mode");
		if (symlink(real_models_dir.c_str(), symlink_models_dir.c_str()) == 0) {
			const auto result = howdy::native::CheckUserModelReadiness(
			    symlink_models_dir, "readiness-user", std::nullopt);
			ok &= expect(result.status == UserModelStatus::kInsecurePath,
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
			const auto result = howdy::native::CheckUserModelReadiness(
			    dangling_models_dir, "readiness-user", std::nullopt);
			ok &= expect(result.status == UserModelStatus::kInsecurePath,
			             "readiness rejects dangling symlinked models directory");
			fs::remove(dangling_models_dir, ec);
			ec.clear();
		} else {
			std::cerr << "SKIP: dangling readiness models directory symlink creation failed\n";
		}

		fs::create_directories(models_dir, ec);
		ok &= expect(!ec, "create explicit readiness models directory");
		ok &= expect(chmod(models_dir.c_str(), 0755) == 0,
		             "set explicit readiness models directory mode");
		{
			const auto result =
			    howdy::native::CheckUserModelReadiness(models_dir, "readiness-user", std::nullopt);
			ok &= expect(result.status == UserModelStatus::kNoModel,
			             "readiness missing model file returns kNoModel");
		}

		ok &= expect(write_file(model_path, "not-json"), "write readiness model file");
		ok &= expect(chmod(model_path.c_str(), 0644) == 0, "set readiness model file mode");
		{
			const auto result =
			    howdy::native::CheckUserModelReadiness(models_dir, "readiness-user", std::nullopt);
			ok &= expect(result.status == UserModelStatus::kOk,
			             "readiness accepts secure model without parsing JSON");
			ok &= expect(result.path == model_path, "readiness respects explicit models directory");
		}

		const auto symlink_target = models_dir / "readiness-target.dat";
		ok &= expect(write_file(symlink_target, "not-json"), "write readiness symlink target");
		fs::remove(model_path, ec);
		ec.clear();
		if (symlink(symlink_target.c_str(), model_path.c_str()) == 0) {
			const auto result =
			    howdy::native::CheckUserModelReadiness(models_dir, "readiness-user", std::nullopt);
			ok &= expect(result.status == UserModelStatus::kInsecurePath,
			             "readiness rejects symlinked model file");
			ok &= expect(fs::remove(model_path, ec), "remove readiness model symlink");
			ec.clear();
		} else {
			std::cerr << "SKIP: readiness model symlink creation failed\n";
		}

		const auto missing_target = models_dir / "readiness-missing-target.dat";
		if (symlink(missing_target.c_str(), model_path.c_str()) == 0) {
			const auto result =
			    howdy::native::CheckUserModelReadiness(models_dir, "readiness-user", std::nullopt);
			ok &= expect(result.status == UserModelStatus::kInsecurePath,
			             "readiness rejects dangling model symlink");
			ok &= expect(fs::remove(model_path, ec), "remove dangling readiness model symlink");
			ec.clear();
		} else {
			std::cerr << "SKIP: dangling readiness model symlink creation failed\n";
		}

		ok &= expect(write_file(model_path, "not-json"), "restore readiness model after symlink");
		const auto hardlink_path = models_dir / "readiness-hardlink.dat";
		if (link(model_path.c_str(), hardlink_path.c_str()) == 0) {
			const auto result =
			    howdy::native::CheckUserModelReadiness(models_dir, "readiness-user", std::nullopt);
			ok &= expect(result.status == UserModelStatus::kInsecurePath,
			             "readiness rejects hard-linked model file");
			ok &= expect(fs::remove(hardlink_path, ec), "remove readiness hardlink");
			ec.clear();
		} else {
			std::cerr << "SKIP: readiness model hardlink creation failed\n";
		}

		ok &= expect(chmod(model_path.c_str(), 0664) == 0, "make readiness model group-writable");
		ok &= expect(
		    howdy::native::CheckUserModelReadiness(models_dir, "readiness-user", std::nullopt)
		            .status == UserModelStatus::kInsecurePath,
		    "readiness rejects group-writable model file");
		ok &= expect(chmod(model_path.c_str(), 0666) == 0, "make readiness model world-writable");
		ok &= expect(
		    howdy::native::CheckUserModelReadiness(models_dir, "readiness-user", std::nullopt)
		            .status == UserModelStatus::kInsecurePath,
		    "readiness rejects world-writable model file");
		ok &= expect(chmod(model_path.c_str(), 0644) == 0, "restore readiness model file mode");

		ok &= expect(chmod(models_dir.c_str(), 0775) == 0,
		             "make readiness models directory group-writable");
		ok &= expect(
		    howdy::native::CheckUserModelReadiness(models_dir, "readiness-user", std::nullopt)
		            .status == UserModelStatus::kInsecurePath,
		    "readiness rejects group-writable models directory");
		ok &= expect(chmod(models_dir.c_str(), 0777) == 0,
		             "make readiness models directory world-writable");
		ok &= expect(
		    howdy::native::CheckUserModelReadiness(models_dir, "readiness-user", std::nullopt)
		            .status == UserModelStatus::kInsecurePath,
		    "readiness rejects world-writable models directory");
		ok &=
		    expect(chmod(models_dir.c_str(), 0755) == 0, "restore readiness models directory mode");

		fs::remove(model_path, ec);
		ec.clear();
		fs::create_directory(model_path, ec);
		ok &= expect(!ec, "create non-regular readiness model path");
		ok &= expect(
		    howdy::native::CheckUserModelReadiness(models_dir, "readiness-user", std::nullopt)
		            .status == UserModelStatus::kInsecurePath,
		    "readiness rejects non-regular model file");

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

	auto TestUserModelLoading() -> bool;
	auto TestUserModelMutationStart() -> bool;
	auto TestUserModelMutationFailures() -> bool;

}  // namespace howdy::test::user_models
