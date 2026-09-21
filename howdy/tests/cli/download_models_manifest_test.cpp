#include "cli/download_models/internal.hpp"
#include "cli/download_models_test_support.hpp"
#include "model_assets/opencv_model_manifest.hpp"
#include "test_support.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>

namespace howdy::test::download_models {

	using howdy::test::Expect;

	namespace {

		struct PinnedModelArtifact {
			std::string_view filename;
			std::string_view url;
			std::string_view sha256;
			std::uintmax_t   size;
		};

		constexpr std::array kPinnedModelArtifacts = {
		    PinnedModelArtifact{
		        .filename = "face_detection_yunet_2026may.onnx",
		        .url      = "https://github.com/opencv/opencv_zoo/raw/"
		                    "26cc381e4d2594bb9f47a26eb8fd96c94a13660d/models/face_detection_yunet/"
		                    "face_detection_yunet_2026may.onnx",
		        .sha256   = "ebafce4e3c118d6554634be5c27ab333b4c047a9a8c3faf1d7cf93101c22f0f0",
		        .size     = 229738,
		    },
		    PinnedModelArtifact{
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
			++DependenciesState().download_attempts;
			DependenciesState().downloaded_urls.push_back(url);
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
		ok &= Expect(RunTestDownload({.models_dir = manifest_models_dir, .output = manifest_output},
		                             &manifest_exit, howdy::native::OfficialOpencvModels(),
		                             OfficialManifestDownloadFile, OfficialManifestSha256File),
		             "run official manifest downloads");
		std::error_code yunet_size_ec;
		std::error_code sface_size_ec;
		ok &= Expect(
		    manifest_exit == 0 &&
		        DependenciesState().downloaded_urls.size() == kPinnedModelArtifacts.size() &&
		        DependenciesState().downloaded_urls.at(0) == kPinnedModelArtifacts.at(0).url &&
		        DependenciesState().downloaded_urls.at(1) == kPinnedModelArtifacts.at(1).url &&
		        fs::file_size(manifest_models_dir / kPinnedModelArtifacts.at(0).filename,
		                      yunet_size_ec) == kPinnedModelArtifacts.at(0).size &&
		        !yunet_size_ec &&
		        fs::file_size(manifest_models_dir / kPinnedModelArtifacts.at(1).filename,
		                      sface_size_ec) == kPinnedModelArtifacts.at(1).size &&
		        !sface_size_ec,
		    "each official download matches its independently pinned artifact");

		const auto &official_yunet = howdy::native::kOfficialOpenCvModels.at(0);
		const auto &official_sface = howdy::native::kOfficialOpenCvModels.at(1);
		ok &= Expect(official_yunet.filename == kPinnedModelArtifacts.at(0).filename &&
		                 official_yunet.url == kPinnedModelArtifacts.at(0).url &&
		                 official_yunet.size == kPinnedModelArtifacts.at(0).size &&
		                 official_yunet.sha256 == kPinnedModelArtifacts.at(0).sha256,
		             "official YuNet descriptor matches independently pinned artifact");
		ok &= Expect(official_sface.filename == kPinnedModelArtifacts.at(1).filename &&
		                 official_sface.url == kPinnedModelArtifacts.at(1).url &&
		                 official_sface.size == kPinnedModelArtifacts.at(1).size &&
		                 official_sface.sha256 == kPinnedModelArtifacts.at(1).sha256,
		             "official SFace descriptor matches independently pinned artifact");
		ok &= Expect(official_yunet.filename != official_sface.filename &&
		                 official_yunet.url != official_sface.url &&
		                 official_yunet.size != official_sface.size &&
		                 official_yunet.sha256 != official_sface.sha256,
		             "official model descriptors remain distinct");

		fs::remove_all(temp_root, ec);
		return ok;
	}

}  // namespace howdy::test::download_models
