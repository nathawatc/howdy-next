#include "common/user_names.hpp"
#include "storage/user_model_readiness.hpp"
#include "storage/user_models.hpp"

#include <array>
#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <unistd.h>

#include <sys/stat.h>

namespace {

    auto write_file(const std::filesystem::path &path, const std::string &content) -> bool {
        std::ofstream out(path);
        if (!out.is_open()) {
            return false;
        }
        out << content;
        return out.good();
    }

    auto expect(bool condition, const std::string &message) -> bool {
        if (!condition) {
            std::cerr << "FAIL: " << message << "\n";
            return false;
        }
        return true;
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

}  // namespace

auto main() -> int {
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
        ok &=
            expect(result.status == howdy::native::UserModelStatus::kOk, "valid models return kOk");
        ok &= expect(result.stored.encodings.size() == 3, "three valid encodings loaded");
        ok &= expect(result.stored.models.size() == 3, "model metadata count matches encodings");
        ok &= expect(result.stored.models[0].id == 7 && result.stored.models[0].label == "first",
                     "first model metadata preserved");
        ok &= expect(result.stored.models[2].id == 8 && result.stored.models[2].label == "second",
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
        write_file(model_path,
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
    if (symlink(symlink_target.c_str(), model_path.c_str()) == 0) {
        const auto result = howdy::native::load_user_models("alice", backend);
        ok &= expect(result.status == howdy::native::UserModelStatus::kInsecurePath,
                     "symlinked model file is rejected");
        ok &= expect(fs::remove(model_path, ec), "remove symlinked model path");
        ec.clear();
    } else {
        std::cerr << "SKIP: model symlink creation failed\n";
    }
    ok &= expect(
        write_file(model_path,
                   R"([{"id":7,"label":"first","backend":"opencv_dnn_sface","data":[[0.1,0.2]]}])"),
        "restore valid model after symlink");

    fs::remove(model_path, ec);
    ec.clear();
    if (symlink(model_path.c_str(), model_path.c_str()) == 0) {
        const auto result = howdy::native::list_user_model_entries("alice", backend);
        ok &= expect(result.status == howdy::native::UserModelStatus::kInsecurePath,
                     "self-referential model symlink is rejected");
        ok &= expect(unlink(model_path.c_str()) == 0, "remove self-referential model symlink");
        ec.clear();
    } else {
        std::cerr << "SKIP: model symlink loop creation failed\n";
    }
    ok &= expect(
        write_file(model_path,
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

    if (geteuid() != 0) {
        ok &= expect(chmod(model_path.c_str(), 0000) == 0, "make model file unreadable");
        const auto result = howdy::native::load_user_models("alice", backend);
        ok &= expect(result.status != howdy::native::UserModelStatus::kOk,
                     "unreadable model file fails safely");
        ok &= expect(chmod(model_path.c_str(), 0644) == 0, "restore unreadable model file");
    } else {
        std::cerr << "SKIP: unreadable model file check while running as root\n";
    }

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
        if (index > 0) {
            oversized_encoding += ",";
        }
        oversized_encoding += "0.1";
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
        write_file(model_path,
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

    std::string oversized_json = R"json([{"id":1,"label":"large","data":[[0.1]],"padding":")json";
    oversized_json.append((1024 * 1024) + 1, 'x');
    oversized_json += "\"}]";
    ok &= expect(write_file(model_path, oversized_json), "write oversized model JSON");
    {
        const auto result = howdy::native::list_user_model_entries("alice", backend);
        ok &= expect(result.status == howdy::native::UserModelStatus::kOversized,
                     "lifecycle listing rejects oversized model JSON");
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

    ok &= expect(write_file(model_path, "not-json"), "write malformed model before verified clear");
    {
        const auto inspection = howdy::native::inspect_user_model_file("alice");
        ok &= expect(inspection.status == howdy::native::UserModelStatus::kOk &&
                         inspection.snapshot.has_value(),
                     "verified clear inspects malformed JSON without parsing");
        if (inspection.snapshot.has_value()) {
            const auto result =
                howdy::native::clear_user_model_entries_if_unchanged("alice", *inspection.snapshot);
            ok &= expect(result.status == howdy::native::UserModelStatus::kOk,
                         "verified clear removes unchanged malformed JSON");
        }
        ok &= expect(!fs::exists(model_path), "verified clear deletes malformed JSON model file");
    }

    ok &= expect(write_file(model_path, oversized_json),
                 "write oversized model before verified clear");
    {
        const auto inspection = howdy::native::inspect_user_model_file("alice");
        ok &= expect(inspection.status == howdy::native::UserModelStatus::kOk &&
                         inspection.snapshot.has_value(),
                     "verified clear inspects oversized JSON without parsing");
        if (inspection.snapshot.has_value()) {
            const auto result =
                howdy::native::clear_user_model_entries_if_unchanged("alice", *inspection.snapshot);
            ok &= expect(result.status == howdy::native::UserModelStatus::kOk,
                         "verified clear removes unchanged oversized JSON");
        }
        ok &= expect(!fs::exists(model_path), "verified clear deletes oversized JSON model file");
    }

    ok &= expect(write_file(model_path, R"({"id":1})"),
                 "write wrong-shape model before verified clear");
    {
        const auto inspection = howdy::native::inspect_user_model_file("alice");
        ok &= expect(inspection.status == howdy::native::UserModelStatus::kOk &&
                         inspection.snapshot.has_value(),
                     "verified clear inspects wrong-shape JSON without parsing");
        if (inspection.snapshot.has_value()) {
            const auto result =
                howdy::native::clear_user_model_entries_if_unchanged("alice", *inspection.snapshot);
            ok &= expect(result.status == howdy::native::UserModelStatus::kOk,
                         "verified clear removes unchanged wrong-shape JSON");
        }
        ok &= expect(!fs::exists(model_path), "verified clear deletes wrong-shape JSON model file");
    }

    ok &= expect(write_file(model_path, "not-json"), "write model before stale verified clear");
    {
        const auto inspection = howdy::native::inspect_user_model_file("alice");
        ok &= expect(inspection.status == howdy::native::UserModelStatus::kOk &&
                         inspection.snapshot.has_value(),
                     "verified clear captures file snapshot");
        ok &=
            expect(write_file(model_path, "changed-json"), "rewrite model after clear inspection");
        if (inspection.snapshot.has_value()) {
            const auto result =
                howdy::native::clear_user_model_entries_if_unchanged("alice", *inspection.snapshot);
            ok &= expect(result.status == howdy::native::UserModelStatus::kModelChanged,
                         "verified clear aborts when model file changes after inspection");
        }
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
        ok &= expect(write_file(model_path, R"([{"id":0,"time":1,"label":"two","data":[[1.0]]}])"),
                     "rewrite model with same-size content after clear inspection");
        if (inspection.snapshot.has_value()) {
            const std::array<timespec, 2> times{
                timespec{.tv_sec  = inspection.snapshot->mtime_seconds,
                         .tv_nsec = inspection.snapshot->mtime_nanosecs},
                timespec{.tv_sec  = inspection.snapshot->mtime_seconds,
                         .tv_nsec = inspection.snapshot->mtime_nanosecs},
            };
            ok &= expect(utimensat(AT_FDCWD, model_path.c_str(), times.data(), 0) == 0,
                         "restore old model mtime after same-size rewrite");
            const auto result =
                howdy::native::clear_user_model_entries_if_unchanged("alice", *inspection.snapshot);
            ok &= expect(result.status == howdy::native::UserModelStatus::kModelChanged,
                         "verified clear detects same-size rewrite with restored mtime");
        }
        ok &= expect(fs::exists(model_path), "stale same-size verified clear leaves model file");
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
        const auto result = howdy::native::append_user_model_entry("alice", default_label_entry);
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

    fs::remove(model_path, ec);
    ec.clear();
    const howdy::native::NewUserModelEntry first_entry{
        .label     = "first",
        .backend   = backend,
        .metric    = "cosine",
        .model     = "sface.onnx",
        .encodings = {{0.1F, 0.2F}},
    };
    const auto first_append = howdy::native::append_user_model_entry("alice", first_entry);
    ok &= expect(first_append.status == howdy::native::UserModelStatus::kOk,
                 "append creates first model entry");
    ok &= expect(first_append.entry.id == 0, "append allocates first model ID");
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
        const auto result = howdy::native::append_user_model_entry("alice", invalid_encoding_entry);
        ok &= expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
                     "append rejects invalid new-entry encodings before writing");
        std::ifstream     persisted(model_path);
        const std::string persisted_text((std::istreambuf_iterator<char>(persisted)),
                                         std::istreambuf_iterator<char>());
        ok &= expect(persisted_text.find("invalid") == std::string::npos,
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
        ok &= expect(persisted_text.find("future_field") != std::string::npos,
                     "append preserves unknown fields in existing entries");
    }

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
        const auto result = howdy::native::remove_user_model_entry_if_matches("alice", expected);
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
        const auto result   = howdy::native::remove_user_model_entry_if_matches("alice", expected);
        ok &= expect(result.status == howdy::native::UserModelStatus::kOk && !result.removed_last,
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
            R"([{"id":0,"time":1,"label":"first","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.1,0.2]],"future_field":"preserved"},{"id":1,"time":1,"label":"second","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.3,0.4]]}])"),
        "restore entries before legacy remove");
    {
        const auto result = howdy::native::remove_user_model_entry("alice", 0);
        ok &= expect(result.status == howdy::native::UserModelStatus::kOk && !result.removed_last,
                     "remove deletes existing model ID");
        ok &= expect(result.entry.id == 0 && result.entry.label == "first",
                     "remove returns actual removed entry");
        const auto remaining = howdy::native::list_user_model_entries("alice", backend);
        ok &= expect(remaining.entries.size() == 1 && remaining.entries[0].id == 1,
                     "remove preserves other model entries");
    }
    {
        const auto result = howdy::native::remove_user_model_entry("alice", 1);
        ok &= expect(result.status == howdy::native::UserModelStatus::kOk && result.removed_last,
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

    const auto lock_path = fs::path(model_path.string() + ".lock");
    fs::remove(lock_path, ec);
    ec.clear();
    if (symlink("/tmp", lock_path.c_str()) == 0) {
        const auto result = howdy::native::append_user_model_entry("alice", first_entry);
        ok &= expect(result.status == howdy::native::UserModelStatus::kLockFailed,
                     "append fails closed when model lock cannot be acquired");
        ok &= expect(fs::remove(lock_path, ec), "remove lock failure symlink");
        ec.clear();
    } else {
        std::cerr << "SKIP: lock symlink creation failed\n";
    }

    fs::remove_all(temp_root, ec);
    unsetenv("HOWDY_USER_MODELS_DIR");

    if (!ok) {
        return 1;
    }
    return 0;
}
