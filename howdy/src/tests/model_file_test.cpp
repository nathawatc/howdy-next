#include "common/file_security.hpp"
#include "common/model_file.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>

#include <sys/stat.h>

namespace {

	constexpr auto kFixture = "known model fixture";

	auto write_file(const std::filesystem::path &path, const std::string_view content) -> bool {
		std::ofstream output(path, std::ios::binary);
		output.write(content.data(), static_cast<std::streamsize>(content.size()));
		return output.good();
	}

	auto expect(const bool condition, const std::string &message) -> bool {
		if (!condition) {
			std::cerr << "FAIL: " << message << "\n";
		}
		return condition;
	}

	auto readiness(const std::filesystem::path &path, const std::string_view label = "Model file")
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

	const auto valid = temp_root / "valid.onnx";
	ok &= expect(write_file(valid, kFixture), "write matching model");
	ok &= expect(readiness(valid).status == OpenCvModelStatus::kOk,
	             "secure non-placeholder model is ready");

	const auto empty = temp_root / "empty.onnx";
	ok &= expect(write_file(empty, ""), "write empty model");
	ok &= expect(readiness(empty).status == OpenCvModelStatus::kInvalid, "empty model is invalid");
	const auto empty_readiness = readiness(empty);
	ok &= expect(empty_readiness.error_message.contains("empty"),
	             "empty model diagnostic identifies empty file");

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
	ok &= expect(html_readiness.error_message.contains("Custom model") &&
	                 html_readiness.error_message.contains(html.string()),
	             "placeholder diagnostic includes label and path");

	const auto xml = temp_root / "xml.onnx";
	ok &= expect(write_file(xml, "<?xml version=\"1.0\"?><error>download failed</error>"),
	             "write XML placeholder");
	ok &=
	    expect(readiness(xml).status == OpenCvModelStatus::kInvalid, "XML placeholder is invalid");

	const auto markup = temp_root / "markup.onnx";
	ok &= expect(write_file(markup, " \t\r\n<response>download failed</response>"),
	             "write generic markup placeholder");
	ok &= expect(readiness(markup).status == OpenCvModelStatus::kInvalid,
	             "trimmed generic markup placeholder is invalid");

	const auto missing = temp_root / "missing.onnx";
	ok &= expect(readiness(missing).status == OpenCvModelStatus::kMissing,
	             "missing model remains missing");

	const auto insecure = temp_root / "insecure.onnx";
	ok &= expect(write_file(insecure, kFixture), "write insecure model");
	ok &= expect(chmod(insecure.c_str(), 0664) == 0, "make model group-writable");
	ok &= expect(readiness(insecure).status == OpenCvModelStatus::kInsecure,
	             "group-writable model remains insecure");

	const auto directory = temp_root / "directory.onnx";
	fs::create_directory(directory, ec);
	ok &= expect(!ec && readiness(directory).status == OpenCvModelStatus::kInsecure,
	             "directory used as model remains insecure");

	const auto insecure_parent = temp_root / "group-writable-parent";
	const auto insecure_child  = insecure_parent / "model.onnx";
	fs::create_directory(insecure_parent, ec);
	ok &= expect(!ec && write_file(insecure_child, kFixture) &&
	                 chmod(insecure_parent.c_str(), 0775) == 0,
	             "create group-writable model parent");
	ok &= expect(readiness(insecure_child).status == OpenCvModelStatus::kInsecure,
	             "group-writable model parent remains insecure");

	const uid_t supplied_owner = getuid() == 0 ? static_cast<uid_t>(1) : static_cast<uid_t>(0);
	const auto  owner_mismatch = howdy::native::check_secure_path(
	    valid, howdy::native::SecurePathKind::kRegularFile, "Model file", supplied_owner);
	ok &= expect(!owner_mismatch.ok &&
	                 owner_mismatch.error_message.contains("UID " + std::to_string(supplied_owner)),
	             "owner mismatch reports supplied UID");

	const auto symlink = temp_root / "symlink.onnx";
	fs::create_symlink(valid, symlink, ec);
	ok &= expect(!ec && readiness(symlink).status == OpenCvModelStatus::kInsecure,
	             "symlink model remains insecure");

	const auto hard_link_source = temp_root / "hard-link-source.onnx";
	const auto hard_link        = temp_root / "hard-link.onnx";
	ok &= expect(write_file(hard_link_source, kFixture), "create hard-link source");
	fs::create_hard_link(hard_link_source, hard_link, ec);
	ok &= expect(!ec && readiness(hard_link).status == OpenCvModelStatus::kInsecure,
	             "hard-linked model remains insecure");

	fs::remove_all(temp_root, ec);
	return ok ? 0 : 1;
}
