#include "cli/download_models_test_support.hpp"
#include "test_support.hpp"

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>

#include <sys/stat.h>

namespace howdy::test::download_models {

	using howdy::test::expect;

	namespace {

		struct PinnedModelArtifact {
			howdy::native::OpenCvModelType type;
			std::string_view               filename;
			std::string_view               url;
			std::string_view               sha256;
			std::uintmax_t                 size;
		};

		constexpr std::array kPinnedModelArtifacts = {
		    PinnedModelArtifact{
		        .type     = howdy::native::OpenCvModelType::kYunet,
		        .filename = "face_detection_yunet_2026may.onnx",
		        .url      = "https://github.com/opencv/opencv_zoo/raw/"
		                    "26cc381e4d2594bb9f47a26eb8fd96c94a13660d/models/face_detection_yunet/"
		                    "face_detection_yunet_2026may.onnx",
		        .sha256   = "ebafce4e3c118d6554634be5c27ab333b4c047a9a8c3faf1d7cf93101c22f0f0",
		        .size     = 229738,
		    },
		    PinnedModelArtifact{
		        .type     = howdy::native::OpenCvModelType::kSface,
		        .filename = "face_recognition_sface_2021dec_int8.onnx",
		        .url      = "https://github.com/opencv/opencv_zoo/raw/"
		                    "088c3571ec70df15100a5e4c26894d95951e92e9/models/face_recognition_sface/"
		                    "face_recognition_sface_2021dec_int8.onnx",
		        .sha256   = "2b0e941e6f16cc048c20aee0c8e31f569118f65d702914540f7bfdc14048d78a",
		        .size     = 9896933,
		    },
		};

		auto OfficialManifestDownloadFile(
		    const std::string                                           &url,
		    howdy::native::download_models_internal::StagedDownloadFile &staged) -> bool {
			++download_attempts;
			downloaded_urls.push_back(url);
			for (const auto &artifact : kPinnedModelArtifacts) {
				if (artifact.url == url) {
					return ftruncate(staged.fd.Get(), static_cast<off_t>(artifact.size)) == 0;
				}
			}
			return false;
		}

		auto OfficialManifestSha256File(const int fd) -> std::optional<std::string> {
			struct stat stat_buf{};
			if (fstat(fd, &stat_buf) != 0 || stat_buf.st_size < 0) {
				return std::nullopt;
			}
			for (const auto &artifact : kPinnedModelArtifacts) {
				if (std::cmp_equal(stat_buf.st_size, artifact.size)) {
					return std::string(artifact.sha256);
				}
			}
			return std::nullopt;
		}

	}  // namespace

	auto RunDownloadModelsManifestTests() -> bool {
		namespace fs              = std::filesystem;
		bool            ok        = true;
		const auto      temp_root = fs::current_path() / "howdy-download-models-test";
		std::error_code ec;

		const auto manifest_models_dir = temp_root / "manifest-models";
		const auto manifest_output     = temp_root / "manifest-output.txt";
		int        manifest_exit       = 0;
		ok &= expect(RunTestDownload({.models_dir = manifest_models_dir, .output = manifest_output},
		                             &manifest_exit, howdy::native::OfficialOpencvModels(),
		                             OfficialManifestDownloadFile, OfficialManifestSha256File),
		             "run official manifest downloads");
		std::error_code yunet_size_ec;
		std::error_code sface_size_ec;
		ok &= expect(manifest_exit == 0 && downloaded_urls.size() == kPinnedModelArtifacts.size() &&
		                 downloaded_urls[0] == kPinnedModelArtifacts[0].url &&
		                 downloaded_urls[1] == kPinnedModelArtifacts[1].url &&
		                 fs::file_size(manifest_models_dir / kPinnedModelArtifacts[0].filename,
		                               yunet_size_ec) == kPinnedModelArtifacts[0].size &&
		                 !yunet_size_ec &&
		                 fs::file_size(manifest_models_dir / kPinnedModelArtifacts[1].filename,
		                               sface_size_ec) == kPinnedModelArtifacts[1].size &&
		                 !sface_size_ec,
		             "each official download matches its independently pinned artifact");

		const auto &official_yunet = howdy::native::kOfficialOpenCvModels[0];
		const auto &official_sface = howdy::native::kOfficialOpenCvModels[1];
		ok &= expect(official_yunet.type == kPinnedModelArtifacts[0].type &&
		                 official_yunet.filename == kPinnedModelArtifacts[0].filename &&
		                 official_yunet.url == kPinnedModelArtifacts[0].url &&
		                 official_yunet.size == kPinnedModelArtifacts[0].size &&
		                 official_yunet.sha256 == kPinnedModelArtifacts[0].sha256,
		             "official YuNet descriptor matches independently pinned artifact");
		ok &= expect(official_sface.type == kPinnedModelArtifacts[1].type &&
		                 official_sface.filename == kPinnedModelArtifacts[1].filename &&
		                 official_sface.url == kPinnedModelArtifacts[1].url &&
		                 official_sface.size == kPinnedModelArtifacts[1].size &&
		                 official_sface.sha256 == kPinnedModelArtifacts[1].sha256,
		             "official SFace descriptor matches independently pinned artifact");
		ok &= expect(official_yunet.filename != official_sface.filename &&
		                 official_yunet.url != official_sface.url &&
		                 official_yunet.size != official_sface.size &&
		                 official_yunet.sha256 != official_sface.sha256,
		             "official model descriptors remain distinct");

		fs::remove_all(temp_root, ec);
		return ok;
	}

}  // namespace howdy::test::download_models
