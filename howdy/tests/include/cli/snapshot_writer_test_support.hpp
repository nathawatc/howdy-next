#pragma once

#include "cli/snapshot/internal.hpp"
#include "support/bit_flags.hpp"
#include "test_support.hpp"

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <unistd.h>

#include <opencv2/core/exception.hpp>
#include <opencv2/core/hal/interface.h>
#include <opencv2/core/mat.hpp>

#include <sys/stat.h>

namespace howdy::test::snapshot_writer {

	inline constexpr mode_t kSnapshotDirectoryMode =
	    S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP;
	inline constexpr mode_t kSnapshotFileMode = S_IRUSR | S_IWUSR;

	struct WriterCallbackContext {
		bool create_encoded_output = true;
		bool throw_cv_exception    = false;
		bool throw_std_exception   = false;
		bool encode_result         = true;

		int encode_calls = 0;

		std::string        received_extension;
		std::vector<uchar> encoded_output;
	};

	inline auto MakeTempRoot(const std::string &name, bool &ok) -> std::filesystem::path {
		const auto      root = std::filesystem::current_path() /
		                       ("howdy-snapshot-writer-" + name + "-" + std::to_string(getpid()));
		std::error_code ec;
		std::filesystem::remove_all(root, ec);
		ok &= howdy::test::Expect(!ec, "remove stale temp root for " + name);
		std::filesystem::create_directories(root, ec);
		ok &= howdy::test::Expect(!ec, "create temp root for " + name);
		ok &= howdy::test::Expect(chmod(root.c_str(), kSnapshotDirectoryMode) == 0,
		                          "secure temp root for " + name);
		return root;
	}

	inline auto CleanupTempRoot(const std::filesystem::path &root, bool ok) -> bool {
		if (!ok) {
			std::cerr << "Leaving snapshot writer test artifacts: " << root << "\n";
			return false;
		}
		std::error_code ec;
		std::filesystem::remove_all(root, ec);
		return howdy::test::Expect(!ec, "remove temp root " + root.string());
	}

	inline auto PathMode(const std::filesystem::path &path) -> std::optional<mode_t> {
		struct stat file_stat{};
		if (stat(path.c_str(), &file_stat) != 0) {
			return std::nullopt;
		}
		return static_cast<mode_t>(howdy::native::MaskedBits(file_stat.st_mode, 0777));
	}

	inline auto ExpectMode(const std::filesystem::path &path, mode_t expected_mode,
	                       const std::string &message) -> bool {
		const auto mode = PathMode(path);
		return howdy::test::Expect(mode.has_value() && *mode == expected_mode, message);
	}

	inline auto CountStagedFiles(const std::filesystem::path &directory) -> std::size_t {
		std::error_code ec;
		if (!std::filesystem::is_directory(directory, ec) || ec) {
			return 0;
		}
		return howdy::test::CountFilesWithPrefix(directory, ".howdy-snapshot-");
	}

	inline auto TinyFrames() -> std::vector<cv::Mat> {
		return {
		    cv::Mat(8, 8, CV_8UC3, cv::Scalar(10, 20, 30)),
		    cv::Mat(8, 8, CV_8UC3, cv::Scalar(40, 50, 60)),
		    cv::Mat(8, 8, CV_8UC3, cv::Scalar(70, 80, 90)),
		    cv::Mat(8, 8, CV_8UC3, cv::Scalar(100, 110, 120)),
		};
	}

	inline auto TextLines() -> std::vector<std::string> {
		return {"snapshot writer test"};
	}

	inline auto CandidatePathForTest(const std::filesystem::path &base, std::size_t collision_index)
	    -> std::filesystem::path {
		if (collision_index == 0) {
			return base;
		}
		return base.parent_path() / (base.stem().string() + "-" + std::to_string(collision_index) +
		                             base.extension().string());
	}

	inline auto FakeEncode(void *raw_context, std::string_view extension, const cv::Mat &image,
	                       std::vector<uchar> *encoded) -> bool {
		(void)image;
		auto *context = static_cast<WriterCallbackContext *>(raw_context);
		++context->encode_calls;
		context->received_extension = extension;
		if (context->throw_cv_exception) {
			throw cv::Exception(cv::Error::StsError, "fake encoder exception", "fake_encode",
			                    __FILE__, __LINE__);
		}
		if (context->throw_std_exception) {
			throw std::runtime_error("fake encoder exception");
		}
		if (context->create_encoded_output) {
			context->encoded_output = {0x01, 0x02, 0x03};
			encoded->insert(encoded->end(), context->encoded_output.begin(),
			                context->encoded_output.end());
		}
		return context->encode_result;
	}

	inline auto FakeDependencies(WriterCallbackContext &context, const std::filesystem::path &root)
	    -> howdy::native::snapshot_internal::SnapshotWriterDependencies {
		return {.context = &context, .encode_image = FakeEncode, .validation_root = {root}};
	}

	auto RunSnapshotWriterSecurityTests() -> bool;

}  // namespace howdy::test::snapshot_writer
