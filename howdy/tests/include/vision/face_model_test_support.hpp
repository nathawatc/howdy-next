#pragma once

#include "model_assets/model_file.hpp"
#include "test_support.hpp"
#include "vision/face_model_test_access.hpp"

#include <filesystem>
#include <new>
#include <stdexcept>
#include <string>

#include <opencv2/core/cvstd_wrapper.hpp>
#include <opencv2/core/exception.hpp>
#include <opencv2/core/mat.hpp>
#include <opencv2/core/types.hpp>
#include <opencv2/objdetect/face.hpp>

namespace howdy::test::face_model {

	inline constexpr auto kYunetSecretPath = "/secret/models/yunet.onnx";
	inline constexpr auto kSfaceSecretPath = "/secret/models/sface.onnx";
	inline constexpr auto kRawError        = "raw backend failure";
	inline constexpr auto kOpenCvSource    = "/opencv/modules/objdetect/src/face_detect.cpp";
	inline constexpr auto kOpenCvFunction  = "FaceDetectorYNImpl::detect";
	inline constexpr auto kDnnEngineEnv    = "OPENCV_FORCE_DNN_ENGINE";

	using ScopedEnvironment = howdy::test::ScopedEnvironmentVariable;

	[[noreturn]] inline void ThrowCvError() {
		throw cv::Exception(cv::Error::StsError, kRawError, kOpenCvFunction, kOpenCvSource, 73);
	}

	[[noreturn]] inline void ThrowCvOutOfMemory() {
		throw cv::Exception(cv::Error::StsNoMem, kRawError, kOpenCvFunction, kOpenCvSource, 74);
	}

	[[noreturn]] inline void ThrowBadAlloc() {
		throw std::bad_alloc();
	}

	[[noreturn]] inline void ThrowStdError() {
		throw std::runtime_error(kRawError);
	}

	class FakeDetector final : public cv::FaceDetectorYN {
	public:
		void setInputSize(const cv::Size &input_size) override {
			input_size_ = input_size;
		}

		auto getInputSize() -> cv::Size override {
			return input_size_;
		}

		void setScoreThreshold(float score_threshold) override {
			score_threshold_ = score_threshold;
		}

		auto getScoreThreshold() -> float override {
			return score_threshold_;
		}

		void setNMSThreshold(float nms_threshold) override {
			nms_threshold_ = nms_threshold;
		}

		auto getNMSThreshold() -> float override {
			return nms_threshold_;
		}

		void setTopK(int top_k) override {
			top_k_ = top_k;
		}

		auto getTopK() -> int override {
			return top_k_;
		}

		auto detect([[maybe_unused]] cv::InputArray image, cv::OutputArray faces) -> int override {
			faces.release();
			return 0;
		}

	private:
		cv::Size input_size_{320, 320};
		float    score_threshold_ = 0.9F;
		float    nms_threshold_   = 0.3F;
		int      top_k_           = 5000;
	};

	class FakeRecognizer final : public cv::FaceRecognizerSF {
	public:
		// OpenCV FaceRecognizerSF::alignCrop requires two adjacent cv::InputArray parameters.
		// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
		void alignCrop([[maybe_unused]] cv::InputArray src_img,
		               [[maybe_unused]] cv::InputArray face_box,
		               cv::OutputArray                 aligned) const override {
			aligned.release();
		}

		void feature([[maybe_unused]] cv::InputArray aligned_img,
		             cv::OutputArray                 feature) override {
			feature.release();
		}

		[[nodiscard]] auto match([[maybe_unused]] cv::InputArray face_feature1,
		                         [[maybe_unused]] cv::InputArray face_feature2,
		                         [[maybe_unused]] int dis_type) const -> double override {
			return 0.0;
		}
	};

	class ThrowingRecognizer final : public cv::FaceRecognizerSF {
	public:
		// OpenCV FaceRecognizerSF::alignCrop requires two adjacent cv::InputArray parameters.
		// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
		void alignCrop([[maybe_unused]] cv::InputArray  src_img,
		               [[maybe_unused]] cv::InputArray  face_box,
		               [[maybe_unused]] cv::OutputArray aligned) const override {
			ThrowStdError();
		}

		void feature([[maybe_unused]] cv::InputArray  aligned_img,
		             [[maybe_unused]] cv::OutputArray feature) override {}

		[[nodiscard]] auto match([[maybe_unused]] cv::InputArray face_feature1,
		                         [[maybe_unused]] cv::InputArray face_feature2,
		                         [[maybe_unused]] int dis_type) const -> double override {
			return 0.0;
		}
	};

	inline auto SuccessfulBackend() -> howdy::native::FaceModelTestAccess::Backend {
		return {
		    .check_readiness =
		        [](const std::filesystem::path &) -> howdy::native::OpenCvModelReadiness {
			    return howdy::native::OpenCvModelReadiness{
			        .status = howdy::native::OpenCvModelStatus::kOk,
			    };
		    },
		    .create_detector = [](const std::string &, const cv::Size &, float, float,
			                      int) -> cv::Ptr<FakeDetector> {
			    return cv::makePtr<FakeDetector>();
		    },
		    .create_recognizer = [](const std::string &) -> cv::Ptr<FakeRecognizer> {
			    return cv::makePtr<FakeRecognizer>();
		    },
		    .set_input_size = [](const cv::Size &) -> void {},
		    .detect         = [](const cv::Mat &, cv::Mat &faces) -> void {
			    faces = cv::Mat{};
		    },
		};
	}

	auto RunFaceModelEnvironmentTests() -> bool;

}  // namespace howdy::test::face_model
