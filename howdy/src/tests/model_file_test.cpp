#include "common/model_file.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>

#include <sys/stat.h>

namespace {

    auto write_file(const std::filesystem::path &path, const std::string &content) -> bool {
        std::ofstream out(path, std::ios::binary);
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

    auto readiness(const std::filesystem::path &path, std::string_view label = "Model file")
        -> howdy::native::OpenCvModelReadiness {
        return howdy::native::check_opencv_model_readiness_with_label(path, label, std::nullopt);
    }

}  // namespace

auto main() -> int {
    namespace fs = std::filesystem;
    using howdy::native::OpenCvModelStatus;

    bool            ok        = true;
    const auto      temp_root = fs::current_path() / "howdy-model-file-test";
    std::error_code ec;
    fs::remove_all(temp_root, ec);
    fs::create_directories(temp_root, ec);
    ok &= expect(!ec, "create temp root");

    const auto missing = temp_root / "missing.onnx";
    ok &= expect(readiness(missing).status == OpenCvModelStatus::kMissing,
                 "missing model is reported missing");

    const auto valid = temp_root / "valid.onnx";
    ok &= expect(write_file(valid, "valid-looking ONNX data"), "write valid-looking model");
    ok &= expect(readiness(valid).status == OpenCvModelStatus::kOk,
                 "valid-looking secure model is accepted");

    const auto lfs_pointer = temp_root / "lfs-pointer.onnx";
    ok &= expect(write_file(lfs_pointer, "version https://git-lfs.github.com/spec/v1\n"),
                 "write Git LFS pointer");
    ok &= expect(readiness(lfs_pointer).status == OpenCvModelStatus::kInvalid,
                 "Git LFS pointer is invalid");

    const auto html = temp_root / "html.onnx";
    ok &= expect(write_file(html, "<html>download error</html>"), "write HTML placeholder");
    const auto html_readiness = readiness(html, "Custom model");
    ok &=
        expect(html_readiness.status == OpenCvModelStatus::kInvalid, "HTML placeholder is invalid");
    ok &= expect(html_readiness.error_message.contains("Custom model"),
                 "custom label appears in diagnostic");

    const auto directory = temp_root / "directory.onnx";
    fs::create_directory(directory, ec);
    ok &= expect(!ec, "create directory instead of model");
    ok &= expect(readiness(directory).status == OpenCvModelStatus::kInsecure,
                 "directory instead of model is rejected");

    const auto group_writable = temp_root / "group-writable.onnx";
    ok &= expect(write_file(group_writable, "model data"), "write group-writable model");
    ok &= expect(chmod(group_writable.c_str(), 0664) == 0, "make model group-writable");
    ok &= expect(readiness(group_writable).status == OpenCvModelStatus::kInsecure,
                 "group-writable model is insecure");

    const auto insecure_dir   = temp_root / "group-writable-dir";
    const auto insecure_model = insecure_dir / "model.onnx";
    fs::create_directory(insecure_dir, ec);
    ok &= expect(!ec, "create model directory");
    ok &= expect(write_file(insecure_model, "model data"), "write model under directory");
    ok &= expect(chmod(insecure_dir.c_str(), 0775) == 0, "make model directory group-writable");
    ok &= expect(readiness(insecure_model).status == OpenCvModelStatus::kInsecure,
                 "group-writable model directory is insecure");

    fs::remove_all(temp_root, ec);
    return ok ? 0 : 1;
}
