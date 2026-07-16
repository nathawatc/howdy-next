#include "storage/user_model_limits.hpp"
#include "storage/user_model_readiness.hpp"
#include "storage/user_model_store.hpp"
#include "storage/user_model_store_test_hooks.hpp"
#include "storage/user_models.hpp"
#include "support/user_names.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <latch>
#include <limits>
#include <string>
#include <thread>
#include <unistd.h>

#include <sys/stat.h>

namespace {

	using howdy::test::expect;

	auto write_file(const std::filesystem::path &path, const std::string &content) -> bool {
		std::ofstream out(path);
		if (!out.is_open()) {
			return false;
		}
		out << content;
		return out.good();
	}

	auto read_file(const std::filesystem::path &path) -> std::string {
		std::ifstream input(path, std::ios::binary);
		return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	}

	auto nested_array(std::size_t depth) -> std::string {
		return std::string(depth, '[') + "0" + std::string(depth, ']');
	}

	auto expectation_from_entry(const howdy::native::UserModelEntry &entry)
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

	auto replace_with_victim_symlink_after_lock(PostLockSymlinkSwap         *swap,
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

	auto replace_with_regular_file_after_lock(PostLockRegularFileSwap     *swap,
	                                          const std::filesystem::path &path) -> void {
		++swap->calls;
		if (rename(swap->replacement_path.c_str(), path.c_str()) != 0) {
			return;
		}
		swap->swapped = true;
	}

	auto install_replacement_before_lock(AbaRegularFileSwap          *swap,
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

	auto restore_original_after_lock(AbaRegularFileSwap *swap, const std::filesystem::path &path)
	    -> void {
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

	auto replace_path_preserving_original(PreservingRegularFileSwap   *swap,
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

	auto expect_readiness_checks(const std::filesystem::path &temp_root) -> bool {
		namespace fs = std::filesystem;
		using howdy::native::UserModelStatus;

		bool            ok = true;
		std::error_code ec;
		const auto      models_dir = temp_root / "explicit-readiness-models";
		const auto      model_path = models_dir / "readiness-user.dat";

		fs::remove_all(models_dir, ec);
		ec.clear();
		{
			const auto result = howdy::native::check_user_model_readiness(
			    models_dir, "readiness-user", std::nullopt);
			ok &= expect(result.status == UserModelStatus::kNoModelDirectory,
			             "readiness missing model directory returns kNoModelDirectory");
		}
		{
			const auto result =
			    howdy::native::check_user_model_readiness(models_dir, "../alice", std::nullopt);
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
			const auto result = howdy::native::check_user_model_readiness(
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
			const auto result = howdy::native::check_user_model_readiness(
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
			const auto result = howdy::native::check_user_model_readiness(
			    models_dir, "readiness-user", std::nullopt);
			ok &= expect(result.status == UserModelStatus::kNoModel,
			             "readiness missing model file returns kNoModel");
		}

		ok &= expect(write_file(model_path, "not-json"), "write readiness model file");
		ok &= expect(chmod(model_path.c_str(), 0644) == 0, "set readiness model file mode");
		{
			const auto result = howdy::native::check_user_model_readiness(
			    models_dir, "readiness-user", std::nullopt);
			ok &= expect(result.status == UserModelStatus::kOk,
			             "readiness accepts secure model without parsing JSON");
			ok &= expect(result.path == model_path, "readiness respects explicit models directory");
		}

		const auto symlink_target = models_dir / "readiness-target.dat";
		ok &= expect(write_file(symlink_target, "not-json"), "write readiness symlink target");
		fs::remove(model_path, ec);
		ec.clear();
		if (symlink(symlink_target.c_str(), model_path.c_str()) == 0) {
			const auto result = howdy::native::check_user_model_readiness(
			    models_dir, "readiness-user", std::nullopt);
			ok &= expect(result.status == UserModelStatus::kInsecurePath,
			             "readiness rejects symlinked model file");
			ok &= expect(fs::remove(model_path, ec), "remove readiness model symlink");
			ec.clear();
		} else {
			std::cerr << "SKIP: readiness model symlink creation failed\n";
		}

		const auto missing_target = models_dir / "readiness-missing-target.dat";
		if (symlink(missing_target.c_str(), model_path.c_str()) == 0) {
			const auto result = howdy::native::check_user_model_readiness(
			    models_dir, "readiness-user", std::nullopt);
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
			const auto result = howdy::native::check_user_model_readiness(
			    models_dir, "readiness-user", std::nullopt);
			ok &= expect(result.status == UserModelStatus::kInsecurePath,
			             "readiness rejects hard-linked model file");
			ok &= expect(fs::remove(hardlink_path, ec), "remove readiness hardlink");
			ec.clear();
		} else {
			std::cerr << "SKIP: readiness model hardlink creation failed\n";
		}

		ok &= expect(chmod(model_path.c_str(), 0664) == 0, "make readiness model group-writable");
		ok &= expect(
		    howdy::native::check_user_model_readiness(models_dir, "readiness-user", std::nullopt)
		            .status == UserModelStatus::kInsecurePath,
		    "readiness rejects group-writable model file");
		ok &= expect(chmod(model_path.c_str(), 0666) == 0, "make readiness model world-writable");
		ok &= expect(
		    howdy::native::check_user_model_readiness(models_dir, "readiness-user", std::nullopt)
		            .status == UserModelStatus::kInsecurePath,
		    "readiness rejects world-writable model file");
		ok &= expect(chmod(model_path.c_str(), 0644) == 0, "restore readiness model file mode");

		ok &= expect(chmod(models_dir.c_str(), 0775) == 0,
		             "make readiness models directory group-writable");
		ok &= expect(
		    howdy::native::check_user_model_readiness(models_dir, "readiness-user", std::nullopt)
		            .status == UserModelStatus::kInsecurePath,
		    "readiness rejects group-writable models directory");
		ok &= expect(chmod(models_dir.c_str(), 0777) == 0,
		             "make readiness models directory world-writable");
		ok &= expect(
		    howdy::native::check_user_model_readiness(models_dir, "readiness-user", std::nullopt)
		            .status == UserModelStatus::kInsecurePath,
		    "readiness rejects world-writable models directory");
		ok &=
		    expect(chmod(models_dir.c_str(), 0755) == 0, "restore readiness models directory mode");

		fs::remove(model_path, ec);
		ec.clear();
		fs::create_directory(model_path, ec);
		ok &= expect(!ec, "create non-regular readiness model path");
		ok &= expect(
		    howdy::native::check_user_model_readiness(models_dir, "readiness-user", std::nullopt)
		            .status == UserModelStatus::kInsecurePath,
		    "readiness rejects non-regular model file");

		fs::remove_all(models_dir, ec);
		return ok;
	}

	template <typename Value, typename Callback>
	void with_present(const std::optional<Value> &value, Callback callback) {
		if (value.has_value()) {
			callback();
		}
	}

	template <typename Callback>
	void when_supported(bool supported, Callback callback, std::string_view skip_message) {
		if (supported) {
			callback();
		} else {
			std::cerr << "SKIP: " << skip_message << "\n";
		}
	}

	auto staged_user_model_paths(const std::filesystem::path &models_dir)
	    -> std::vector<std::filesystem::path> {
		std::vector<std::filesystem::path> paths;
		for (const auto &entry : std::filesystem::directory_iterator(models_dir)) {
			if (entry.path().filename().string().starts_with(".howdy-user-model-")) {
				paths.push_back(entry.path());
			}
		}
		return paths;
	}

}  // namespace

namespace {

	auto test_user_model_loading() -> bool {
		namespace fs = std::filesystem;

		bool            ok        = true;
		const auto      temp_root = fs::current_path() / "howdy-user-models-test";
		std::error_code ec;
		unlink((temp_root / "models" / "alice.dat").c_str());
		fs::remove_all(temp_root, ec);
		fs::create_directories(temp_root, ec);
		ok &= expect(!ec, "create temp root");

		const auto models_dir = temp_root / "models";
		setenv("HOWDY_USER_MODELS_DIR", models_dir.c_str(), 1);
		ok &= expect_readiness_checks(temp_root);
		{
			const auto result = howdy::native::list_user_model_entries("alice", {});
			ok &= expect(result.status == howdy::native::UserModelStatus::kNoModelDirectory,
			             "missing model directory returns kNoModelDirectory");
		}
		{
			const auto result = howdy::native::load_user_models("alice", "opencv_dnn_sface");
			ok &= expect(result.status == howdy::native::UserModelStatus::kNoModel,
			             "load remaps missing model directory to kNoModel");
		}
		{
			const auto result = howdy::native::inspect_user_model_file("alice");
			ok &= expect(result.status == howdy::native::UserModelStatus::kNoModelDirectory,
			             "inspect returns kNoModelDirectory");
		}
		fs::create_directories(models_dir, ec);
		ok &= expect(!ec, "create models dir");

		const std::string backend = "opencv_dnn_sface";
		ok &= expect(howdy::native::is_valid_model_user_name("alice@example.com"),
		             "domain-style usernames remain valid");
		ok &= expect(!howdy::native::is_valid_model_user_name("../alice"),
		             "malformed path-traversal username input is rejected");
		ok &= expect(!howdy::native::is_valid_model_user_name("alice..bob"),
		             "malformed username containing dot-dot is rejected");
		ok &= expect(!howdy::native::resolve_user_model_path(models_dir, "../alice").has_value(),
		             "unsafe model paths are not constructed");
		ok &= expect(!howdy::native::resolve_user_model_path(models_dir, "alice..bob").has_value(),
		             "dot-dot username input is rejected before model path construction");

		{
			const auto result = howdy::native::load_user_models("../alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kInvalidUser,
			             "invalid username returns kInvalidUser");
		}

		{
			const auto result = howdy::native::load_user_models("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kNoModel,
			             "missing file returns kNoModel");
		}
		{
			const auto result = howdy::native::inspect_user_model_file("alice");
			ok &= expect(result.status == howdy::native::UserModelStatus::kNoModel,
			             "inspect returns kNoModel");
		}

		const auto model_path = models_dir / "alice.dat";
		when_supported(
		    mkfifo(model_path.c_str(), 0600) == 0,
		    [&] -> void {
			    const auto started = std::chrono::steady_clock::now();
			    const auto result  = howdy::native::load_user_models("alice", backend);
			    const auto elapsed = std::chrono::steady_clock::now() - started;
			    ok &= expect(result.status == howdy::native::UserModelStatus::kInsecurePath,
			                 "FIFO model target is rejected");
			    ok &= expect(elapsed < std::chrono::seconds(1),
			                 "FIFO model target is rejected without blocking");
			    ok &= expect(fs::remove(model_path, ec), "remove FIFO model target");
			    ec.clear();
		    },
		    "FIFO model target creation failed");
		ok &= expect(write_file(model_path, "not-json"), "write malformed model file");
		{
			const auto result = howdy::native::inspect_user_model_file("alice");
			ok &= expect(result.status == howdy::native::UserModelStatus::kOk,
			             "inspect accepts malformed JSON without parsing");
		}
		{
			const auto result = howdy::native::load_user_models("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kParseError,
			             "malformed json returns kParseError");
		}

		ok &= expect(write_file(model_path, "[]"), "write empty model list");
		{
			const auto result = howdy::native::load_user_models("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kNoModel,
			             "empty model list returns kNoModel");
		}

		ok &= expect(
		    write_file(model_path,
		               R"([{"id":1,"label":"bad","backend":"other_backend","data":[[0.1,0.2]]}])"),
		    "write incompatible backend model");
		{
			const auto result = howdy::native::load_user_models("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kIncompatibleBackend,
			             "incompatible backend is detected");
		}

		ok &= expect(write_file(model_path,
		                        R"([
{"id":7,"label":"first","backend":"opencv_dnn_sface","data":[[0.1,0.2],[0.3,0.4]]},
{"id":8,"label":"second","backend":"opencv_dnn_sface","data":["ignored",[1.1,1.2,1.3]]}
])"),
		             "write valid models");
		{
			const auto result = howdy::native::load_user_models("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kOk,
			             "valid models return kOk");
			ok &= expect(result.stored.encodings.size() == 3, "three valid encodings loaded");
			ok &=
			    expect(result.stored.models.size() == 3, "model metadata count matches encodings");
			ok &=
			    expect(result.stored.models[0].id == 7 && result.stored.models[0].label == "first",
			           "first model metadata preserved");
			ok &=
			    expect(result.stored.models[2].id == 8 && result.stored.models[2].label == "second",
			           "second model metadata preserved");
		}

		ok &= expect(
		    write_file(
		        model_path,
		        R"([{"id":9,"label":"bad/name","backend":"opencv_dnn_sface","data":[[0.1,0.2]]}])"),
		    "write unsafe label model");
		{
			const auto result = howdy::native::load_user_models("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kParseError,
			             "model label containing path separator is rejected");
		}

		ok &= expect(
		    write_file(
		        model_path,
		        R"([{"id":9,"label":"bad\nname","backend":"opencv_dnn_sface","data":[[0.1,0.2]]}])"),
		    "write control-character label model");
		{
			const auto result = howdy::native::load_user_models("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kParseError,
			             "model label containing newline is rejected");
		}

		ok &= expect(
		    write_file(
		        model_path,
		        R"([{"id":7,"label":"first","backend":"opencv_dnn_sface","data":[[0.1,0.2]]}])"),
		    "restore valid model after unsafe label");

		const auto symlink_target = models_dir / "target.dat";
		ok &= expect(
		    write_file(
		        symlink_target,
		        R"([{"id":10,"label":"target","backend":"opencv_dnn_sface","data":[[0.1,0.2]]}])"),
		    "write symlink target model");
		fs::remove(model_path, ec);
		ec.clear();
		when_supported(
		    symlink(symlink_target.c_str(), model_path.c_str()) == 0,
		    [&] -> void {
			    const auto result = howdy::native::load_user_models("alice", backend);
			    ok &= expect(result.status == howdy::native::UserModelStatus::kInsecurePath,
			                 "symlinked model file is rejected");
			    ok &= expect(fs::remove(model_path, ec), "remove symlinked model path");
			    ec.clear();
		    },
		    "model symlink creation failed");
		ok &= expect(
		    write_file(
		        model_path,
		        R"([{"id":7,"label":"first","backend":"opencv_dnn_sface","data":[[0.1,0.2]]}])"),
		    "restore valid model after symlink");

		fs::remove(model_path, ec);
		ec.clear();
		when_supported(
		    symlink(model_path.c_str(), model_path.c_str()) == 0,
		    [&] -> void {
			    const auto result = howdy::native::list_user_model_entries("alice", backend);
			    ok &= expect(result.status == howdy::native::UserModelStatus::kInsecurePath,
			                 "self-referential model symlink is rejected");
			    ok &= expect(unlink(model_path.c_str()) == 0,
			                 "remove self-referential model symlink");
			    ec.clear();
		    },
		    "model symlink loop creation failed");
		ok &= expect(
		    write_file(
		        model_path,
		        R"([{"id":7,"label":"first","backend":"opencv_dnn_sface","data":[[0.1,0.2]]}])"),
		    "restore valid model after symlink loop");

		ok &= expect(chmod(model_path.c_str(), 0664) == 0, "make model file group-writable");
		{
			const auto result = howdy::native::load_user_models("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kInsecurePath,
			             "group-writable model file is rejected");
		}
		ok &= expect(chmod(model_path.c_str(), 0644) == 0, "restore model file mode");

		ok &= expect(chmod(model_path.c_str(), 0666) == 0, "make model file world-writable");
		{
			const auto result = howdy::native::load_user_models("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kInsecurePath,
			             "world-writable model file is rejected");
		}
		ok &= expect(chmod(model_path.c_str(), 0644) == 0, "restore model file mode");

		const auto hardlink_path = models_dir / "alice-hardlink.dat";
		ok &= expect(fs::remove(hardlink_path, ec) || !ec, "remove stale hardlink path");
		ec.clear();
		ok &= expect(link(model_path.c_str(), hardlink_path.c_str()) == 0,
		             "create hard-linked model file");
		{
			const auto result = howdy::native::load_user_models("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kInsecurePath,
			             "hard-linked model file is rejected");
		}
		ok &= expect(fs::remove(hardlink_path, ec), "remove hard-linked model file");
		ec.clear();

		when_supported(
		    geteuid() != 0,
		    [&] -> void {
			    ok &= expect(chmod(model_path.c_str(), 0000) == 0, "make model file unreadable");
			    const auto result = howdy::native::load_user_models("alice", backend);
			    ok &= expect(result.status != howdy::native::UserModelStatus::kOk,
			                 "unreadable model file fails safely");
			    ok &= expect(chmod(model_path.c_str(), 0644) == 0, "restore unreadable model file");
		    },
		    "unreadable model file check while running as root");

		ok &= expect(chmod(models_dir.c_str(), 0775) == 0, "make models dir group-writable");
		{
			const auto result = howdy::native::load_user_models("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kInsecurePath,
			             "group-writable model dir is rejected");
		}
		ok &= expect(chmod(models_dir.c_str(), 0755) == 0, "restore models dir mode");

		ok &= expect(chmod(models_dir.c_str(), 0777) == 0, "make models dir world-writable");
		{
			const auto result = howdy::native::load_user_models("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kInsecurePath,
			             "world-writable model dir is rejected");
		}
		ok &= expect(chmod(models_dir.c_str(), 0755) == 0, "restore models dir mode");

		std::string oversized_encoding =
		    R"([{"id":9,"label":"oversized","backend":"opencv_dnn_sface","data":[[)";
		for (int index = 0; index < 1100; ++index) {
			oversized_encoding += index > 0 ? ",0.1" : "0.1";
		}
		oversized_encoding += "]]}]";
		ok &= expect(write_file(model_path, oversized_encoding), "write oversized encoding");
		{
			const auto result = howdy::native::load_user_models("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kParseError,
			             "oversized encoding is rejected");
		}

		ok &= expect(write_file(model_path, R"({"id":1})"), "write wrong top-level JSON shape");
		{
			const auto result = howdy::native::list_user_model_entries("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "lifecycle listing rejects wrong top-level JSON shape");
		}
		{
			const auto result = howdy::native::load_user_models("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kParseError,
			             "load remaps invalid-shape model JSON to kParseError");
		}

		ok &= expect(
		    write_file(
		        model_path,
		        R"([{"id":-1,"time":1,"label":"bad","backend":"opencv_dnn_sface","data":[[0.1]]}])"),
		    "write negative ID model");
		{
			const auto result = howdy::native::list_user_model_entries("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "lifecycle listing rejects negative model IDs");
		}

		ok &= expect(
		    write_file(
		        model_path,
		        R"([{"id":1,"time":1,"label":"first","backend":"opencv_dnn_sface","data":[[0.1]]},{"id":1,"time":2,"label":"second","backend":"opencv_dnn_sface","data":[[0.2]]}])"),
		    "write duplicate ID models");
		{
			const auto result = howdy::native::list_user_model_entries("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "lifecycle listing rejects duplicate model IDs");
		}

		ok &= expect(
		    write_file(
		        model_path,
		        R"([{"id":"1","time":1,"label":"bad","backend":"opencv_dnn_sface","data":[[0.1]]}])"),
		    "write non-integer ID model");
		{
			const auto result = howdy::native::list_user_model_entries("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "lifecycle listing rejects non-integer model IDs");
		}

		ok &= expect(
		    write_file(
		        model_path,
		        R"([{"id":2147483647,"time":1,"label":"bad","backend":"opencv_dnn_sface","data":[[0.1]]}])"),
		    "write max-int ID model");
		{
			const auto result = howdy::native::list_user_model_entries("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "lifecycle listing rejects max-int model IDs");
		}

		ok &= expect(
		    write_file(
		        model_path,
		        R"([{"id":1,"time":"now","label":"bad","backend":"opencv_dnn_sface","data":[[0.1]]}])"),
		    "write non-integer timestamp model");
		{
			const auto result = howdy::native::list_user_model_entries("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "lifecycle listing rejects non-integer timestamps");
		}

		ok &= expect(
		    write_file(
		        model_path,
		        R"([{"id":1,"time":1,"label":7,"backend":"opencv_dnn_sface","data":[[0.1]]}])"),
		    "write non-string label model");
		{
			const auto result = howdy::native::list_user_model_entries("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "lifecycle listing rejects non-string labels");
		}

		ok &= expect(
		    write_file(
		        model_path,
		        R"([{"id":1,"time":1,"label":"bad","backend":"opencv_dnn_sface","data":[[0.1,"bad"]]}])"),
		    "write non-numeric encoding value model");
		{
			const auto result = howdy::native::list_user_model_entries("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "lifecycle listing rejects non-numeric encoding values");
		}

		ok &= expect(
		    write_file(
		        model_path,
		        R"([{"id":1,"time":1,"label":"bad","backend":"opencv_dnn_sface","data":[[0.1,[0.2]]]}])"),
		    "write nested encoding array model");
		{
			const auto result = howdy::native::list_user_model_entries("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "lifecycle listing rejects nested encoding arrays");
		}

		ok &= expect(
		    write_file(
		        model_path,
		        R"([{"id":1,"time":1,"label":"bad","backend":"opencv_dnn_sface","data":[[0.1,{}]]}])"),
		    "write nested encoding object model");
		{
			const auto result = howdy::native::list_user_model_entries("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "lifecycle listing rejects nested encoding objects");
		}

		ok &= expect(
		    write_file(
		        model_path,
		        R"([{"id":1,"time":1,"label":"bad","backend":"opencv_dnn_sface","data":[[1e999]]}])"),
		    "write non-finite encoding value model");
		{
			const auto result = howdy::native::list_user_model_entries("alice", backend);
			ok &= expect(result.status != howdy::native::UserModelStatus::kOk,
			             "lifecycle listing rejects non-finite encoding values when representable");
		}

		ok &= expect(
		    write_file(
		        model_path,
		        R"([{"id":1,"label":"bad","backend":"opencv_dnn_sface","metric":"l2","model":"sface.onnx","data":[[0.1]]}])"),
		    "write incompatible metric model");
		{
			const auto result =
			    howdy::native::list_user_model_entries("alice", backend, "cosine", "sface.onnx");
			ok &= expect(result.status == howdy::native::UserModelStatus::kIncompatibleMetric,
			             "lifecycle listing rejects incompatible metric");
		}
		{
			const auto result =
			    howdy::native::list_user_model_entries("alice", backend, "l2", "other.onnx");
			ok &= expect(result.status == howdy::native::UserModelStatus::kIncompatibleModel,
			             "lifecycle listing rejects incompatible model metadata");
		}

		std::string oversized_json =
		    R"json([{"id":1,"label":"large","data":[[0.1]],"padding":")json";
		oversized_json.append((1024 * 1024) + 1, 'x');
		oversized_json += "\"}]";
		ok &= expect(write_file(model_path, oversized_json), "write oversized model JSON");
		{
			const auto result = howdy::native::list_user_model_entries("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kOversized,
			             "lifecycle listing rejects oversized model JSON");
		}
		{
			const auto result = howdy::native::load_user_models("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kParseError,
			             "load remaps oversized model JSON to kParseError");
		}

		fs::remove(model_path, ec);
		ec.clear();
		{
			const auto result = howdy::native::clear_user_model_entries("alice");
			ok &= expect(result.status == howdy::native::UserModelStatus::kNoModel,
			             "clear reports no model file without parsing");
		}

		ok &= expect(write_file(model_path, "not-json"), "write malformed model before clear");
		{
			const auto result = howdy::native::clear_user_model_entries("alice");
			ok &= expect(result.status == howdy::native::UserModelStatus::kOk,
			             "clear removes malformed JSON");
			ok &= expect(!fs::exists(model_path), "clear deletes malformed JSON model file");
		}

		ok &= expect(write_file(model_path, oversized_json), "write oversized model before clear");
		{
			const auto result = howdy::native::clear_user_model_entries("alice");
			ok &= expect(result.status == howdy::native::UserModelStatus::kOk,
			             "clear removes oversized JSON");
			ok &= expect(!fs::exists(model_path), "clear deletes oversized JSON model file");
		}

		ok &= expect(write_file(model_path, R"({"id":1})"), "write wrong-shape model before clear");
		{
			const auto result = howdy::native::clear_user_model_entries("alice");
			ok &= expect(result.status == howdy::native::UserModelStatus::kOk,
			             "clear removes wrong-shape JSON");
			ok &= expect(!fs::exists(model_path), "clear deletes wrong-shape JSON model file");
		}

		ok &= expect(write_file(model_path, "not-json"),
		             "write malformed model before verified clear");
		{
			const auto inspection = howdy::native::inspect_user_model_file("alice");
			ok &= expect(inspection.status == howdy::native::UserModelStatus::kOk &&
			                 inspection.snapshot.has_value(),
			             "verified clear inspects malformed JSON without parsing");
			with_present(inspection.snapshot, [&] -> void {
				const auto result = howdy::native::clear_user_model_entries_if_unchanged(
				    "alice", *inspection.snapshot);
				ok &= expect(result.status == howdy::native::UserModelStatus::kOk,
				             "verified clear removes unchanged malformed JSON");
			});
			ok &=
			    expect(!fs::exists(model_path), "verified clear deletes malformed JSON model file");
		}

		ok &= expect(write_file(model_path, oversized_json),
		             "write oversized model before verified clear");
		{
			const auto inspection = howdy::native::inspect_user_model_file("alice");
			ok &= expect(inspection.status == howdy::native::UserModelStatus::kOk &&
			                 inspection.snapshot.has_value(),
			             "verified clear inspects oversized JSON without parsing");
			with_present(inspection.snapshot, [&] -> void {
				const auto result = howdy::native::clear_user_model_entries_if_unchanged(
				    "alice", *inspection.snapshot);
				ok &= expect(result.status == howdy::native::UserModelStatus::kOk,
				             "verified clear removes unchanged oversized JSON");
			});
			ok &=
			    expect(!fs::exists(model_path), "verified clear deletes oversized JSON model file");
		}

		ok &= expect(write_file(model_path, R"({"id":1})"),
		             "write wrong-shape model before verified clear");
		{
			const auto inspection = howdy::native::inspect_user_model_file("alice");
			ok &= expect(inspection.status == howdy::native::UserModelStatus::kOk &&
			                 inspection.snapshot.has_value(),
			             "verified clear inspects wrong-shape JSON without parsing");
			with_present(inspection.snapshot, [&] -> void {
				const auto result = howdy::native::clear_user_model_entries_if_unchanged(
				    "alice", *inspection.snapshot);
				ok &= expect(result.status == howdy::native::UserModelStatus::kOk,
				             "verified clear removes unchanged wrong-shape JSON");
			});
			ok &= expect(!fs::exists(model_path),
			             "verified clear deletes wrong-shape JSON model file");
		}

		ok &= expect(write_file(model_path, "not-json"), "write model before stale verified clear");
		{
			const auto inspection = howdy::native::inspect_user_model_file("alice");
			ok &= expect(inspection.status == howdy::native::UserModelStatus::kOk &&
			                 inspection.snapshot.has_value(),
			             "verified clear captures file snapshot");
			ok &= expect(write_file(model_path, "changed-json"),
			             "rewrite model after clear inspection");
			with_present(inspection.snapshot, [&] -> void {
				const auto result = howdy::native::clear_user_model_entries_if_unchanged(
				    "alice", *inspection.snapshot);
				ok &= expect(result.status == howdy::native::UserModelStatus::kModelChanged,
				             "verified clear aborts when model file changes after inspection");
			});
			ok &= expect(fs::exists(model_path), "stale verified clear leaves changed model file");
		}

		ok &= expect(write_file(model_path, R"([{"id":0,"time":1,"label":"one","data":[[1.0]]}])"),
		             "write same-size model before stale clear");
		{
			const auto inspection = howdy::native::inspect_user_model_file("alice");
			ok &= expect(inspection.status == howdy::native::UserModelStatus::kOk &&
			                 inspection.snapshot.has_value(),
			             "verified clear captures snapshot before same-size rewrite");
			std::this_thread::sleep_for(std::chrono::milliseconds(1100));
			ok &= expect(
			    write_file(model_path, R"([{"id":0,"time":1,"label":"two","data":[[1.0]]}])"),
			    "rewrite model with same-size content after clear inspection");
			with_present(inspection.snapshot, [&] -> void {
				const std::array<timespec, 2> times{
				    timespec{.tv_sec  = inspection.snapshot->mtime_seconds,
				             .tv_nsec = inspection.snapshot->mtime_nanosecs},
				    timespec{.tv_sec  = inspection.snapshot->mtime_seconds,
				             .tv_nsec = inspection.snapshot->mtime_nanosecs},
				};
				ok &= expect(utimensat(AT_FDCWD, model_path.c_str(), times.data(), 0) == 0,
				             "restore old model mtime after same-size rewrite");
				const auto result = howdy::native::clear_user_model_entries_if_unchanged(
				    "alice", *inspection.snapshot);
				ok &= expect(result.status == howdy::native::UserModelStatus::kModelChanged,
				             "verified clear detects same-size rewrite with restored mtime");
			});
			ok &=
			    expect(fs::exists(model_path), "stale same-size verified clear leaves model file");
		}

		ok &= expect(
		    write_file(
		        model_path,
		        R"([{"id":10,"time":1,"label":"existing","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.1,0.2]]}])"),
		    "write existing model before default-label append");
		{
			const howdy::native::NewUserModelEntry default_label_entry{
			    .label     = "",
			    .backend   = backend,
			    .metric    = "cosine",
			    .model     = "sface.onnx",
			    .encodings = {{0.3F, 0.4F}},
			};
			const auto result =
			    howdy::native::append_user_model_entry("alice", default_label_entry);
			ok &= expect(result.status == howdy::native::UserModelStatus::kOk,
			             "append assigns actual ID from locked storage state");
			ok &= expect(result.entry.id == 11, "append returns actual created model ID");
			ok &= expect(result.entry.label == "Model #11",
			             "append default label matches actual created model ID");
			const auto listing =
			    howdy::native::list_user_model_entries("alice", backend, "cosine", "sface.onnx");
			ok &= expect(listing.status == howdy::native::UserModelStatus::kOk &&
			                 listing.entries.size() == 2 && listing.entries[1].id == 11 &&
			                 listing.entries[1].label == "Model #11",
			             "stored default label matches actual created model ID");
		}
		return ok;
	}

	auto test_user_model_mutation_start() -> bool {
		namespace fs                 = std::filesystem;
		bool              ok         = true;
		const auto        temp_root  = fs::current_path() / "howdy-user-models-test";
		const auto        models_dir = temp_root / "models";
		const auto        model_path = models_dir / "alice.dat";
		const std::string backend    = "opencv_dnn_sface";
		std::error_code   ec;

		fs::remove(model_path, ec);
		ec.clear();
		const howdy::native::NewUserModelEntry first_entry{
		    .label     = "first",
		    .backend   = backend,
		    .metric    = "cosine",
		    .model     = "sface.onnx",
		    .encodings = {{0.1F, 0.2F}},
		};
		{
			const std::string original_model =
			    R"([{"id":0,"time":1,"label":"first","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.1,0.2]]}])";
			const auto victim_path    = temp_root / "post-lock-revalidate-victim.txt";
			const auto victim_content = std::string("victim sentinel\n");
			ok &= expect(
			    write_file(model_path, original_model),
			    "write secure existing model before begin-mutation post-lock revalidation test");
			ok &= expect(write_file(victim_path, victim_content),
			             "write victim file before begin-mutation post-lock revalidation test");

			PostLockSymlinkSwap hook{.victim_path = victim_path};
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .after_lock_before_revalidate = [&hook](const std::filesystem::path &path) -> void {
				    replace_with_victim_symlink_after_lock(&hook, path);
			    },
			});
			const auto result = howdy::native::UserModelStore::begin_mutation("alice");
			ok &= expect(hook.calls == 1,
			             "begin_mutation post-lock revalidation hook executes exactly once");
			ok &= expect(hook.swapped,
			             "begin_mutation post-lock hook atomically replaces model with symlink");
			ok &= expect(result.document.result.status ==
			                 howdy::native::UserModelStatus::kInsecurePath,
			             "begin_mutation post-lock revalidation rejects symlink replacement");
			ok &= expect(!result.transaction.has_value(),
			             "begin_mutation post-lock validation failure creates no transaction");
			ok &= expect(
			    read_file(victim_path) == victim_content,
			    "begin_mutation post-lock validation failure leaves victim content unchanged");
			struct stat replaced_stat{};
			ok &= expect(
			    lstat(model_path.c_str(), &replaced_stat) == 0 && S_ISLNK(replaced_stat.st_mode),
			    "begin_mutation post-lock test replacement remains symlink after failed mutation");
			fs::remove(model_path, ec);
			ec.clear();
			fs::remove(victim_path, ec);
			ec.clear();
		}
		{
			const std::string original_model =
			    R"([{"id":0,"time":1,"label":"first","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.1,0.2]]}])";
			const auto victim_path    = temp_root / "post-lock-lock-existing-victim.txt";
			const auto victim_content = std::string("lock existing victim sentinel\n");
			ok &= expect(
			    write_file(model_path, original_model),
			    "write secure existing model before lock_existing post-lock revalidation test");
			ok &= expect(write_file(victim_path, victim_content),
			             "write victim file before lock_existing post-lock revalidation test");

			PostLockSymlinkSwap hook{.victim_path = victim_path};
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .after_lock_before_revalidate = [&hook](const std::filesystem::path &path) -> void {
				    replace_with_victim_symlink_after_lock(&hook, path);
			    },
			});
			const auto result = howdy::native::UserModelStore::lock_existing("alice");
			ok &= expect(hook.calls == 1,
			             "lock_existing post-lock revalidation hook executes exactly once");
			ok &= expect(hook.swapped,
			             "lock_existing post-lock hook atomically replaces model with symlink");
			ok &= expect(result.status == howdy::native::UserModelStatus::kInsecurePath,
			             "lock_existing post-lock revalidation rejects symlink replacement");
			ok &= expect(!result.transaction.has_value(),
			             "lock_existing post-lock validation failure creates no transaction");
			ok &= expect(
			    read_file(victim_path) == victim_content,
			    "lock_existing post-lock validation failure leaves victim content unchanged");
			struct stat replaced_stat{};
			ok &= expect(
			    lstat(model_path.c_str(), &replaced_stat) == 0 && S_ISLNK(replaced_stat.st_mode),
			    "lock_existing post-lock test replacement remains symlink after failed mutation");
			fs::remove(model_path, ec);
			ec.clear();
			fs::remove(victim_path, ec);
			ec.clear();
		}
		{
			const std::string original_model =
			    R"([{"id":0,"time":1,"label":"first","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.1,0.2]]}])";
			const std::string replacement_model =
			    R"([{"id":0,"time":1,"label":"replacement","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.5,0.6]]}])";
			const auto replacement_path = temp_root / "post-lock-begin-mutation-replacement.dat";
			ok &=
			    expect(write_file(model_path, original_model),
			           "write secure existing model before begin_mutation regular-file swap test");
			ok &= expect(write_file(replacement_path, replacement_model),
			             "write replacement file before begin_mutation regular-file swap test");

			PostLockRegularFileSwap hook{.replacement_path = replacement_path};
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .after_lock_before_revalidate = [&hook](const std::filesystem::path &path) -> void {
				    replace_with_regular_file_after_lock(&hook, path);
			    },
			});
			const auto result = howdy::native::UserModelStore::begin_mutation("alice");
			ok &= expect(hook.calls == 1,
			             "begin_mutation regular-file swap hook executes exactly once");
			ok &= expect(hook.swapped,
			             "begin_mutation hook atomically replaces model with regular file");
			ok &= expect(result.document.result.status ==
			                 howdy::native::UserModelStatus::kModelChanged,
			             "begin_mutation rejects post-lock regular-file inode replacement");
			ok &= expect(!result.transaction.has_value(),
			             "begin_mutation regular-file replacement failure creates no transaction");
			ok &= expect(read_file(model_path) == replacement_model,
			             "begin_mutation regular-file replacement remains unmodified");
			fs::remove(model_path, ec);
			ec.clear();
		}
		{
			const std::string original_model =
			    R"([{"id":0,"time":1,"label":"first","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.1,0.2]]}])";
			const std::string replacement_model =
			    R"([{"id":0,"time":1,"label":"replacement","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.7,0.8]]}])";
			const auto replacement_path = temp_root / "post-lock-lock-existing-replacement.dat";
			ok &= expect(write_file(model_path, original_model),
			             "write secure existing model before lock_existing regular-file swap test");
			ok &= expect(write_file(replacement_path, replacement_model),
			             "write replacement file before lock_existing regular-file swap test");

			PostLockRegularFileSwap hook{.replacement_path = replacement_path};
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .after_lock_before_revalidate = [&hook](const std::filesystem::path &path) -> void {
				    replace_with_regular_file_after_lock(&hook, path);
			    },
			});
			const auto result = howdy::native::UserModelStore::lock_existing("alice");
			ok &= expect(hook.calls == 1,
			             "lock_existing regular-file swap hook executes exactly once");
			ok &= expect(hook.swapped,
			             "lock_existing hook atomically replaces model with regular file");
			ok &= expect(result.status == howdy::native::UserModelStatus::kModelChanged,
			             "lock_existing rejects post-lock regular-file inode replacement");
			ok &= expect(!result.transaction.has_value(),
			             "lock_existing regular-file replacement failure creates no transaction");
			ok &= expect(read_file(model_path) == replacement_model,
			             "lock_existing regular-file replacement remains unmodified");
			fs::remove(model_path, ec);
			ec.clear();
		}
		{
			const std::string original_model =
			    R"([{"id":0,"time":1,"label":"first","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.1,0.2]]}])";
			const std::string replacement_model =
			    R"([{"id":0,"time":1,"label":"aba-replacement","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.9,1.0]]}])";
			AbaRegularFileSwap hook{
			    .replacement_path              = temp_root / "begin-mutation-aba-b.dat",
			    .original_saved_path           = temp_root / "begin-mutation-aba-a-saved.dat",
			    .locked_replacement_saved_path = temp_root / "begin-mutation-aba-b-locked.dat",
			};
			ok &= expect(write_file(model_path, original_model),
			             "write A before begin_mutation ABA swap test");
			ok &= expect(write_file(hook.replacement_path, replacement_model),
			             "write B before begin_mutation ABA swap test");
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .before_lock = [&hook](const std::filesystem::path &path) -> void {
				    install_replacement_before_lock(&hook, path);
			    },
			    .after_lock_before_revalidate = [&hook](const std::filesystem::path &path) -> void {
				    restore_original_after_lock(&hook, path);
			    },
			});
			const auto result = howdy::native::UserModelStore::begin_mutation("alice");
			ok &= expect(hook.before_lock_calls == 1 && hook.after_lock_calls == 1,
			             "begin_mutation ABA hooks execute exactly once");
			ok &=
			    expect(hook.installed_b && hook.restored_a,
			           "begin_mutation ABA test installs B before lock and restores A after lock");
			ok &= expect(result.document.result.status ==
			                 howdy::native::UserModelStatus::kModelChanged,
			             "begin_mutation rejects ABA inode replacement");
			ok &= expect(!result.transaction.has_value(),
			             "begin_mutation ABA rejection creates no transaction");
			ok &= expect(read_file(model_path) == original_model,
			             "begin_mutation ABA restored original remains unmodified");
			ok &= expect(read_file(hook.locked_replacement_saved_path) == replacement_model,
			             "begin_mutation ABA locked replacement remains unmodified");
			fs::remove(model_path, ec);
			ec.clear();
			fs::remove(hook.locked_replacement_saved_path, ec);
			ec.clear();
		}
		{
			const std::string original_model =
			    R"([{"id":0,"time":1,"label":"first","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.1,0.2]]}])";
			const std::string replacement_model =
			    R"([{"id":0,"time":1,"label":"aba-replacement","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[1.1,1.2]]}])";
			AbaRegularFileSwap hook{
			    .replacement_path              = temp_root / "lock-existing-aba-b.dat",
			    .original_saved_path           = temp_root / "lock-existing-aba-a-saved.dat",
			    .locked_replacement_saved_path = temp_root / "lock-existing-aba-b-locked.dat",
			};
			ok &= expect(write_file(model_path, original_model),
			             "write A before lock_existing ABA swap test");
			ok &= expect(write_file(hook.replacement_path, replacement_model),
			             "write B before lock_existing ABA swap test");
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .before_lock = [&hook](const std::filesystem::path &path) -> void {
				    install_replacement_before_lock(&hook, path);
			    },
			    .after_lock_before_revalidate = [&hook](const std::filesystem::path &path) -> void {
				    restore_original_after_lock(&hook, path);
			    },
			});
			const auto result = howdy::native::UserModelStore::lock_existing("alice");
			ok &= expect(hook.before_lock_calls == 1 && hook.after_lock_calls == 1,
			             "lock_existing ABA hooks execute exactly once");
			ok &= expect(hook.installed_b && hook.restored_a,
			             "lock_existing ABA test installs B before lock and restores A after lock");
			ok &= expect(result.status == howdy::native::UserModelStatus::kModelChanged,
			             "lock_existing rejects ABA inode replacement");
			ok &= expect(!result.transaction.has_value(),
			             "lock_existing ABA rejection creates no transaction");
			ok &= expect(read_file(model_path) == original_model,
			             "lock_existing ABA restored original remains unmodified");
			ok &= expect(read_file(hook.locked_replacement_saved_path) == replacement_model,
			             "lock_existing ABA locked replacement remains unmodified");
			fs::remove(model_path, ec);
			ec.clear();
			fs::remove(hook.locked_replacement_saved_path, ec);
			ec.clear();
		}
		ok &= expect(write_file(model_path, "not-json"),
		             "write malformed existing file before mutations");
		const auto malformed_before_mutation = read_file(model_path);
		{
			const auto result = howdy::native::append_user_model_entry("alice", first_entry);
			ok &= expect(result.status == howdy::native::UserModelStatus::kParseError,
			             "append rejects malformed existing file");
			ok &= expect(read_file(model_path) == malformed_before_mutation,
			             "failed append leaves malformed existing file unchanged");
		}
		{
			const auto result = howdy::native::remove_user_model_entry("alice", 0);
			ok &= expect(result.status == howdy::native::UserModelStatus::kParseError,
			             "remove rejects malformed existing file");
			ok &= expect(read_file(model_path) == malformed_before_mutation,
			             "failed remove leaves malformed existing file unchanged");
		}
		ok &= expect(write_file(model_path, R"({"id":1})"),
		             "write invalid-shape existing file before mutations");
		const auto invalid_shape_before_mutation = read_file(model_path);
		{
			const auto result = howdy::native::append_user_model_entry("alice", first_entry);
			ok &= expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "append rejects invalid-shape existing file");
			ok &= expect(read_file(model_path) == invalid_shape_before_mutation,
			             "failed append leaves invalid-shape existing file unchanged");
		}
		{
			const auto result = howdy::native::remove_user_model_entry("alice", 0);
			ok &= expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "remove rejects invalid-shape existing file");
			ok &= expect(read_file(model_path) == invalid_shape_before_mutation,
			             "failed remove leaves invalid-shape existing file unchanged");
		}
		const auto deeply_nested_model =
		    R"([{"id":0,"time":1,"label":"deep","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.1]],"unknown":)" +
		    nested_array(howdy::native::user_model_limits::kMaxJsonNestingDepth) + "}]";
		ok &= expect(write_file(model_path, deeply_nested_model),
		             "write deeply nested unknown field before mutations");
		const auto deeply_nested_before_mutation = read_file(model_path);
		{
			const auto result = howdy::native::append_user_model_entry("alice", first_entry);
			ok &= expect(result.status == howdy::native::UserModelStatus::kOversized,
			             "append rejects deeply nested unknown field");
			ok &= expect(read_file(model_path) == deeply_nested_before_mutation,
			             "deep nesting append failure leaves model file unchanged");
		}
		{
			const auto result = howdy::native::remove_user_model_entry("alice", 0);
			ok &= expect(result.status == howdy::native::UserModelStatus::kOversized,
			             "remove rejects deeply nested unknown field");
			ok &= expect(read_file(model_path) == deeply_nested_before_mutation,
			             "deep nesting remove failure leaves model file unchanged");
		}
		fs::remove(model_path, ec);
		ec.clear();
		const auto first_append = howdy::native::append_user_model_entry("alice", first_entry);
		ok &= expect(first_append.status == howdy::native::UserModelStatus::kOk,
		             "append creates first model entry");
		ok &= expect(first_append.entry.id == 0, "append allocates first model ID");
		{
			const auto                             before_oversized_append = read_file(model_path);
			const howdy::native::NewUserModelEntry oversized_entry{
			    .label = std::string(static_cast<std::size_t>(
			                             howdy::native::user_model_limits::kMaxUserModelFileBytes),
			                         'x'),
			    .backend   = backend,
			    .metric    = "cosine",
			    .model     = "sface.onnx",
			    .encodings = {{0.3F, 0.4F}},
			};
			const auto result = howdy::native::append_user_model_entry("alice", oversized_entry);
			ok &= expect(result.status == howdy::native::UserModelStatus::kWriteFailed,
			             "append rejects serialized model larger than read limit");
			ok &= expect(read_file(model_path) == before_oversized_append,
			             "oversized append leaves existing model file unchanged");
		}
		ok &= expect(
		    write_file(
		        model_path,
		        R"([{"id":0,"time":1,"label":"first","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.1,0.2]],"future_field":"preserved"}])"),
		    "add unknown field to existing model entry");

		const howdy::native::NewUserModelEntry second_entry{
		    .label     = "second",
		    .backend   = backend,
		    .metric    = "cosine",
		    .model     = "sface.onnx",
		    .encodings = {{0.3F, 0.4F}},
		};
		const howdy::native::NewUserModelEntry invalid_encoding_entry{
		    .label     = "invalid",
		    .backend   = backend,
		    .metric    = "cosine",
		    .model     = "sface.onnx",
		    .encodings = {{std::numeric_limits<float>::infinity()}},
		};
		{
			const auto result =
			    howdy::native::append_user_model_entry("alice", invalid_encoding_entry);
			ok &= expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "append rejects invalid new-entry encodings before writing");
			std::ifstream     persisted(model_path);
			const std::string persisted_text((std::istreambuf_iterator<char>(persisted)),
			                                 std::istreambuf_iterator<char>());
			ok &= expect(!persisted_text.contains("invalid"),
			             "append does not write invalid new-entry encodings");
		}
		const auto second_append = howdy::native::append_user_model_entry("alice", second_entry);
		ok &= expect(second_append.status == howdy::native::UserModelStatus::kOk,
		             "append adds second model entry");
		ok &= expect(second_append.entry.id == 1, "append allocates next model ID");
		{
			const auto result =
			    howdy::native::list_user_model_entries("alice", backend, "cosine", "sface.onnx");
			ok &= expect(result.status == howdy::native::UserModelStatus::kOk,
			             "lifecycle listing loads appended entries");
			ok &= expect(result.entries.size() == 2, "append preserves existing entries");
			ok &= expect(result.entries[0].label == "first" && result.entries[1].label == "second",
			             "lifecycle listing preserves entry order");
			ok &= expect(result.next_id == 2, "lifecycle listing reports next model ID");
			std::ifstream     persisted(model_path);
			const std::string persisted_text((std::istreambuf_iterator<char>(persisted)),
			                                 std::istreambuf_iterator<char>());
			ok &= expect(persisted_text.contains("future_field"),
			             "append preserves unknown fields in existing entries");
		}
		return ok;
	}

	auto test_user_model_mutation_failures() -> bool {
		namespace fs                 = std::filesystem;
		bool              ok         = true;
		const auto        temp_root  = fs::current_path() / "howdy-user-models-test";
		const auto        models_dir = temp_root / "models";
		const auto        model_path = models_dir / "alice.dat";
		const std::string backend    = "opencv_dnn_sface";
		std::error_code   ec;
		const howdy::native::NewUserModelEntry first_entry{
		    .label     = "first",
		    .backend   = backend,
		    .metric    = "cosine",
		    .model     = "sface.onnx",
		    .encodings = {{0.1F, 0.2F}},
		};
		const howdy::native::NewUserModelEntry second_entry{
		    .label     = "second",
		    .backend   = backend,
		    .metric    = "cosine",
		    .model     = "sface.onnx",
		    .encodings = {{0.3F, 0.4F}},
		};
		{
			const auto before_failed_write = read_file(model_path);
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .fail_write = true,
			});
			const auto result = howdy::native::append_user_model_entry("alice", second_entry);
			ok &= expect(result.status == howdy::native::UserModelStatus::kWriteFailed,
			             "append reports deterministic staged write failure");
			ok &= expect(read_file(model_path) == before_failed_write,
			             "staged write failure leaves previous model bytes unchanged");
		}
		{
			const auto before_failed_fsync = read_file(model_path);
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .fail_fsync = true,
			});
			const auto result = howdy::native::append_user_model_entry("alice", second_entry);
			ok &= expect(result.status == howdy::native::UserModelStatus::kWriteFailed,
			             "append reports deterministic staged fsync failure");
			ok &= expect(read_file(model_path) == before_failed_fsync,
			             "staged fsync failure leaves previous model bytes unchanged");
		}
		{
			const auto before_parent_sync_failure = read_file(model_path);
			const auto before =
			    howdy::native::list_user_model_entries("alice", backend, "cosine", "sface.onnx");
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .fail_parent_sync = true,
			});
			const auto result = howdy::native::append_user_model_entry("alice", second_entry);
			const auto after =
			    howdy::native::list_user_model_entries("alice", backend, "cosine", "sface.onnx");
			ok &= expect(result.status == howdy::native::UserModelStatus::kDurabilityUncertain,
			             "append distinguishes committed parent-sync failure");
			ok &= expect(result.error_message.contains("verify state before retrying"),
			             "append parent-sync failure warns before retry");
			ok &= expect(before.status == howdy::native::UserModelStatus::kOk &&
			                 after.status == howdy::native::UserModelStatus::kOk &&
			                 after.entries.size() == before.entries.size() + 1,
			             "append parent-sync failure leaves committed model visible");
			ok &= expect(write_file(model_path, before_parent_sync_failure),
			             "restore model after append parent-sync failure test");
		}
		{
			const auto original_content = read_file(model_path);
			const auto before =
			    howdy::native::list_user_model_entries("alice", backend, "cosine", "sface.onnx");
			{
				const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
				    .fail_write_cleanup = true,
				});
				const auto result = howdy::native::append_user_model_entry("alice", second_entry);
				const auto after  = howdy::native::list_user_model_entries("alice", backend,
				                                                           "cosine", "sface.onnx");
				ok &= expect(result.status == howdy::native::UserModelStatus::kOk,
				             "append reports success after committed write with cleanup failure");
				ok &= expect(before.status == howdy::native::UserModelStatus::kOk &&
				                 after.status == howdy::native::UserModelStatus::kOk &&
				                 after.entries.size() == before.entries.size() + 1,
				             "committed write with cleanup failure remains canonical");
			}
			bool found_write_temp = false;
			for (const auto &entry : fs::directory_iterator(models_dir)) {
				found_write_temp |=
				    entry.path().filename().string().starts_with(".howdy-user-model-");
			}
			ok &= expect(found_write_temp,
			             "write cleanup failure leaves injected temporary artifact");
			const auto cleanup_retry =
			    howdy::native::append_user_model_entry("alice", second_entry);
			ok &= expect(cleanup_retry.status == howdy::native::UserModelStatus::kOk,
			             "next append succeeds after stale write cleanup");
			found_write_temp = false;
			for (const auto &entry : fs::directory_iterator(models_dir)) {
				found_write_temp |=
				    entry.path().filename().string().starts_with(".howdy-user-model-");
			}
			ok &= expect(!found_write_temp, "next append removes stale write artifact");
			fs::remove(model_path, ec);
			ec.clear();
			ok &= expect(write_file(model_path, original_content),
			             "restore model after write cleanup failure test");
		}
		{
			const auto before_swap_during_write = read_file(model_path);
			const auto replacement_path         = temp_root / "swap-during-write-model.dat";
			const auto replacement_model        = std::string(
			    R"([{"id":0,"time":1,"label":"swap-during-write","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.4,0.5]]}])");
			ok &= expect(write_file(replacement_path, replacement_model),
			             "write replacement before swap-during-write test");
			PostLockRegularFileSwap hook{.replacement_path = replacement_path};
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .before_write_commit = [&hook](const std::filesystem::path &path) -> void {
				    replace_with_regular_file_after_lock(&hook, path);
			    },
			});
			const auto result = howdy::native::append_user_model_entry("alice", second_entry);
			ok &= expect(hook.calls == 1 && hook.swapped,
			             "swap-during-write hook atomically replaces model before commit");
			ok &= expect(result.status == howdy::native::UserModelStatus::kWriteFailed,
			             "append rejects swap during staged write commit");
			ok &= expect(read_file(model_path) == replacement_model,
			             "swap-during-write replacement remains unmodified");
			fs::remove(model_path, ec);
			ec.clear();
			ok &= expect(write_file(model_path, before_swap_during_write),
			             "restore model after swap-during-write test");
		}
		{
			const auto before_swap_after_check = read_file(model_path);
			const auto replacement_model       = std::string(
			    R"([{"id":0,"time":1,"label":"swap-after-write-check","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.6,0.7]]}])");
			PreservingRegularFileSwap hook{
			    .replacement_path = temp_root / "swap-after-write-check-replacement.dat",
			    .displaced_path   = temp_root / "swap-after-write-check-original.dat",
			};
			ok &= expect(write_file(hook.replacement_path, replacement_model),
			             "write replacement before post-check write swap test");
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .after_write_identity_check = [&hook](const std::filesystem::path &path) -> void {
				    replace_path_preserving_original(&hook, path);
			    },
			});
			const auto result = howdy::native::append_user_model_entry("alice", second_entry);
			ok &= expect(hook.calls == 1 && hook.swapped,
			             "post-check write hook replaces model after final identity check");
			ok &= expect(result.status == howdy::native::UserModelStatus::kWriteFailed,
			             "append fails when model changes after final write identity check");
			ok &= expect(read_file(model_path) == replacement_model,
			             "failed post-check write leaves replacement model unchanged");
			ok &= expect(read_file(hook.displaced_path) == before_swap_after_check,
			             "failed post-check write leaves displaced locked model unchanged");
			fs::remove(model_path, ec);
			ec.clear();
			fs::remove(hook.displaced_path, ec);
			ec.clear();
			ok &= expect(write_file(model_path, before_swap_after_check),
			             "restore model after post-check write swap test");
		}
		{
			const auto original_content  = read_file(model_path);
			const auto replacement_model = std::string(
			    R"([{"id":0,"time":1,"label":"rollback-failure-replacement","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.8,0.9]]}])");
			PreservingRegularFileSwap hook{
			    .replacement_path = temp_root / "rollback-failure-replacement.dat",
			    .displaced_path   = temp_root / "rollback-failure-original.dat",
			};
			ok &= expect(write_file(hook.replacement_path, replacement_model),
			             "write replacement before rollback-failure test");
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .after_write_identity_check = [&hook](const std::filesystem::path &path) -> void {
				    replace_path_preserving_original(&hook, path);
			    },
			    .fail_write_rollback = true,
			});
			const auto result = howdy::native::append_user_model_entry("alice", second_entry);
			ok &= expect(hook.calls == 1 && hook.swapped,
			             "rollback-failure hook replaces model after final identity check");
			ok &= expect(result.status == howdy::native::UserModelStatus::kCommitStateUncertain,
			             "append distinguishes failed post-commit recovery");
			ok &= expect(result.error_message.contains("inspect state before retrying"),
			             "failed post-commit recovery requires state inspection");
			ok &= expect(result.entry.id == -1 && !result.removed_last,
			             "uncertain commit result makes no mutation-state claim");
			ok &= expect(read_file(model_path) != replacement_model,
			             "failed rollback leaves changed canonical namespace visible");

			const auto staged_paths = staged_user_model_paths(models_dir);
			for (const auto &staged_path : staged_paths) {
				fs::remove(staged_path, ec);
				ec.clear();
			}
			fs::remove(model_path, ec);
			ec.clear();
			fs::remove(hook.displaced_path, ec);
			ec.clear();
			ok &= expect(write_file(model_path, original_content),
			             "restore model after rollback-failure test");
		}
		{
			const auto original_content = read_file(model_path);
			std::latch clear_paused(1);
			std::latch allow_clear(1);
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .before_delete_commit = [&](const std::filesystem::path &) -> void {
				    clear_paused.count_down();
				    allow_clear.wait();
			    },
			});
			howdy::native::UserModelMutationResult                        clear_result;
			std::thread                                                   clearer([&] -> void {
				clear_result = howdy::native::clear_user_model_entries("alice");
			});
			clear_paused.wait();
			const auto during_clear =
			    howdy::native::list_user_model_entries("alice", backend, "cosine", "sface.onnx");
			allow_clear.count_down();
			clearer.join();
			const auto after_clear =
			    howdy::native::list_user_model_entries("alice", backend, "cosine", "sface.onnx");
			ok &= expect(during_clear.status == howdy::native::UserModelStatus::kOk &&
			                 !during_clear.entries.empty(),
			             "reader during clear observes original model document");
			ok &= expect(clear_result.status == howdy::native::UserModelStatus::kOk,
			             "concurrent-read clear succeeds");
			ok &= expect(after_clear.status == howdy::native::UserModelStatus::kNoModel,
			             "reader after clear observes no model");
			ok &= expect(write_file(model_path, original_content),
			             "restore model after concurrent-read clear test");
		}
		{
			const auto original_content = read_file(model_path);
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .fail_delete_unlink = true,
			});
			const auto result = howdy::native::clear_user_model_entries("alice");
			ok &= expect(result.status == howdy::native::UserModelStatus::kDeleteFailed,
			             "clear reports injected unlink failure");
			ok &= expect(read_file(model_path) == original_content,
			             "unlink failure leaves original model at canonical path");
		}
		{
			const auto original_content = read_file(model_path);
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .fail_parent_sync = true,
			});
			const auto result = howdy::native::clear_user_model_entries("alice");
			ok &= expect(result.status == howdy::native::UserModelStatus::kDurabilityUncertain,
			             "clear distinguishes committed parent-sync failure");
			ok &= expect(result.removed_last,
			             "clear parent-sync failure reports committed removal state");
			ok &= expect(!fs::exists(model_path),
			             "clear parent-sync failure leaves model removed from namespace");
			ok &= expect(write_file(model_path, original_content),
			             "restore model after clear parent-sync failure test");
		}
		when_supported(
		    geteuid() != 0,
		    [&] -> void {
			    const auto before_failed_writes = read_file(model_path);
			    ok &= expect(chmod(models_dir.c_str(), 0555) == 0,
			                 "make models directory unwritable for failed-write checks");
			    const auto append_result =
			        howdy::native::append_user_model_entry("alice", second_entry);
			    ok &= expect(append_result.status == howdy::native::UserModelStatus::kWriteFailed,
			                 "append reports atomic write failure");
			    ok &= expect(read_file(model_path) == before_failed_writes,
			                 "failed append write leaves model file unchanged");
			    const auto clear_result = howdy::native::clear_user_model_entries("alice");
			    ok &= expect(clear_result.status == howdy::native::UserModelStatus::kDeleteFailed,
			                 "clear reports failure when parent directory is unwritable");
			    ok &= expect(read_file(model_path) == before_failed_writes,
			                 "failed clear leaves model file unchanged");
			    ok &= expect(chmod(models_dir.c_str(), 0755) == 0,
			                 "restore models directory after failed-write checks");
		    },
		    "atomic write failure checks while running as root");

		{
			const auto result = howdy::native::remove_user_model_entry("alice", 99);
			ok &= expect(result.status == howdy::native::UserModelStatus::kModelNotFound,
			             "remove reports missing model ID");
		}
		{
			const auto listing =
			    howdy::native::list_user_model_entries("alice", backend, "cosine", "sface.onnx");
			ok &= expect(listing.status == howdy::native::UserModelStatus::kOk &&
			                 listing.entries.size() == 2,
			             "list entries before verified stale remove");
			const auto expected = expectation_from_entry(listing.entries[0]);
			ok &= expect(
			    write_file(
			        model_path,
			        R"([{"id":0,"time":2,"label":"changed","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.1,0.2]],"future_field":"preserved"},{"id":1,"time":1,"label":"second","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.3,0.4]]}])"),
			    "rewrite model entry after remove listing");
			const auto result =
			    howdy::native::remove_user_model_entry_if_matches("alice", expected);
			ok &= expect(result.status == howdy::native::UserModelStatus::kModelChanged,
			             "verified remove aborts when model entry changes after listing");
			const auto after =
			    howdy::native::list_user_model_entries("alice", backend, "cosine", "sface.onnx");
			ok &= expect(after.status == howdy::native::UserModelStatus::kOk &&
			                 after.entries.size() == 2 && after.entries[0].label == "changed",
			             "stale verified remove leaves changed model entry");
		}
		ok &= expect(
		    write_file(
		        model_path,
		        R"([{"id":0,"time":1,"label":"first","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.1,0.2]],"future_field":"preserved"},{"id":1,"time":1,"label":"second","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.3,0.4]]}])"),
		    "restore unchanged entries before verified remove");
		{
			const auto listing =
			    howdy::native::list_user_model_entries("alice", backend, "cosine", "sface.onnx");
			ok &= expect(listing.status == howdy::native::UserModelStatus::kOk &&
			                 listing.entries.size() == 2,
			             "list entries before verified remove");
			const auto expected = expectation_from_entry(listing.entries[0]);
			const auto result =
			    howdy::native::remove_user_model_entry_if_matches("alice", expected);
			ok &=
			    expect(result.status == howdy::native::UserModelStatus::kOk && !result.removed_last,
			           "verified remove succeeds when model entry is unchanged");
			ok &= expect(result.entry.id == 0 && result.entry.label == "first",
			             "verified remove returns actual removed entry");
			const auto remaining = howdy::native::list_user_model_entries("alice", backend);
			ok &= expect(remaining.entries.size() == 1 && remaining.entries[0].id == 1,
			             "verified remove preserves other model entries");
		}
		ok &= expect(
		    write_file(
		        model_path,
		        R"([{"id":0,"time":1,"label":"first","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.1,0.2]]},{"id":1,"time":1,"label":"second","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.3,0.4]],"future_field":{"revision":2}}])"),
		    "restore entries before legacy remove");
		{
			const auto result = howdy::native::remove_user_model_entry("alice", 0);
			ok &=
			    expect(result.status == howdy::native::UserModelStatus::kOk && !result.removed_last,
			           "remove deletes existing model ID");
			ok &= expect(result.entry.id == 0 && result.entry.label == "first",
			             "remove returns actual removed entry");
			const auto remaining = howdy::native::list_user_model_entries("alice", backend);
			ok &= expect(remaining.entries.size() == 1 && remaining.entries[0].id == 1,
			             "remove preserves other model entries");
			const auto persisted = read_file(model_path);
			ok &= expect(persisted.contains("\"future_field\"") &&
			                 persisted.contains("\"revision\":2"),
			             "remove preserves unknown fields in another entry");
		}
		{
			const auto result = howdy::native::remove_user_model_entry("alice", 1);
			ok &=
			    expect(result.status == howdy::native::UserModelStatus::kOk && result.removed_last,
			           "remove deletes last model entry");
			ok &= expect(!fs::exists(model_path), "remove last entry deletes model file");
		}

		ok &= expect(howdy::native::append_user_model_entry("alice", first_entry).status ==
		                 howdy::native::UserModelStatus::kOk,
		             "append recreates model before clear");
		{
			const auto result = howdy::native::clear_user_model_entries("alice");
			ok &= expect(result.status == howdy::native::UserModelStatus::kOk,
			             "clear removes all model entries");
			ok &= expect(!fs::exists(model_path), "clear deletes model file");
		}

		const auto created_models_dir = temp_root / "created-store-models";
		fs::remove_all(created_models_dir, ec);
		ec.clear();
		setenv("HOWDY_USER_MODELS_DIR", created_models_dir.c_str(), 1);
		{
			const auto result = howdy::native::append_user_model_entry("created-user", first_entry);
			ok &= expect(result.status == howdy::native::UserModelStatus::kOk,
			             "append creates missing secure models directory");
			struct stat dir_stat{};
			struct stat file_stat{};
			const auto  created_model_path = created_models_dir / "created-user.dat";
			ok &= expect(stat(created_models_dir.c_str(), &dir_stat) == 0,
			             "stat append-created models directory");
			ok &= expect(stat(created_model_path.c_str(), &file_stat) == 0,
			             "stat append-created model file");
			ok &= expect((dir_stat.st_mode & 0777) == 0750,
			             "append-created models directory uses 0750 mode");
			ok &= expect((file_stat.st_mode & 0777) == 0600,
			             "append-created model file uses 0600 mode");
		}
		fs::remove_all(created_models_dir, ec);
		ec.clear();
		setenv("HOWDY_USER_MODELS_DIR", models_dir.c_str(), 1);

		const auto stale_lock_path = fs::path(model_path.string() + ".lock");
		fs::remove(stale_lock_path, ec);
		ec.clear();
		when_supported(
		    symlink("/tmp", stale_lock_path.c_str()) == 0,
		    [&] -> void {
			    const auto result = howdy::native::append_user_model_entry("alice", first_entry);
			    ok &= expect(result.status == howdy::native::UserModelStatus::kOk,
			                 "stale sidecar lock symlink does not affect descriptor locking");
			    ok &= expect(fs::remove(stale_lock_path, ec), "remove stale lock symlink");
			    ec.clear();
		    },
		    "stale lock symlink creation failed");

		fs::remove_all(temp_root, ec);
		unsetenv("HOWDY_USER_MODELS_DIR");
		return ok;
	}

}  // namespace

auto main() -> int {
	const std::array results = {test_user_model_loading(), test_user_model_mutation_start(),
	                            test_user_model_mutation_failures()};
	const bool       ok      = std::ranges::all_of(results, [](bool value) -> bool {
		return value;
	});
	return ok ? 0 : 1;
}
