#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace howdy::native {

	enum class OpenCvModelType : std::uint8_t {
		kYunet,
		kSface,
	};

	struct OpenCvModelDescriptor {
		OpenCvModelType  type;
		std::string_view filename;
		std::string_view url;
		std::string_view sha256;
		std::uintmax_t   size;
	};

	inline constexpr OpenCvModelDescriptor kYunetModelDescriptor{
	    .type     = OpenCvModelType::kYunet,
	    .filename = "face_detection_yunet_2026may.onnx",
	    .url = "https://github.com/opencv/opencv_zoo/raw/26cc381e4d2594bb9f47a26eb8fd96c94a13660d/"
	           "models/face_detection_yunet/face_detection_yunet_2026may.onnx",
	    .sha256 = "ebafce4e3c118d6554634be5c27ab333b4c047a9a8c3faf1d7cf93101c22f0f0",
	    .size   = 229738,
	};

	inline constexpr OpenCvModelDescriptor kSfaceModelDescriptor{
	    .type     = OpenCvModelType::kSface,
	    .filename = "face_recognition_sface_2021dec_int8.onnx",
	    .url = "https://github.com/opencv/opencv_zoo/raw/088c3571ec70df15100a5e4c26894d95951e92e9/"
	           "models/face_recognition_sface/face_recognition_sface_2021dec_int8.onnx",
	    .sha256 = "2b0e941e6f16cc048c20aee0c8e31f569118f65d702914540f7bfdc14048d78a",
	    .size   = 9896933,
	};

	inline constexpr std::array kOfficialOpenCvModels = {
	    kYunetModelDescriptor,
	    kSfaceModelDescriptor,
	};

	[[nodiscard]] constexpr auto official_opencv_models()
	    -> std::span<const OpenCvModelDescriptor> {
		return kOfficialOpenCvModels;
	}

}  // namespace howdy::native
