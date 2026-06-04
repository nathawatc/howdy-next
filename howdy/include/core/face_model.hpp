#pragma once

#include "config/config_reader.hpp"

#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/objdetect/face.hpp>

namespace howdy::native {

    struct FaceMatch {
        int   index    = -1;
        float score    = 0.0F;
        bool  accepted = false;
    };

    class FaceModel {
    public:
        static constexpr auto kBackendName = "opencv_dnn_sface";
        static constexpr auto kYunetModel  = "face_detection_yunet_2023mar_int8bq.onnx";
        static constexpr auto kSfaceModel  = "face_recognition_sface_2021dec_int8bq.onnx";

        explicit FaceModel(const ConfigReader &config);

        [[nodiscard]] auto ok() const -> bool;
        [[nodiscard]] auto error_message() const -> const std::string &;
        [[nodiscard]] auto metric() const -> const std::string &;

        [[nodiscard]] auto prepare_frame(const cv::Mat &frame) const -> cv::Mat;
        auto               detect(const cv::Mat &frame) -> std::vector<cv::Mat>;
        auto               encode(const cv::Mat &frame, const cv::Mat &face) -> std::vector<float>;
        [[nodiscard]] auto best_match(const std::vector<std::vector<float>> &known,
                                      const std::vector<float> &probe) const -> FaceMatch;
        [[nodiscard]] auto detection_box(const cv::Mat &face) const
            -> std::tuple<int, int, int, int>;
        [[nodiscard]] auto detection_landmarks(const cv::Mat &face) const -> std::vector<cv::Point>;
        [[nodiscard]] auto detection_confidence(const cv::Mat &face) const -> float;

    private:
        void               set_input_size_from_frame(const cv::Mat &frame);
        void               set_error(std::string message);
        [[nodiscard]] auto resolve_model_path(const ConfigReader &config, const std::string &option,
                                              const std::string &fallback) const -> std::string;

        bool                          ok_ = false;
        std::string                   error_message_;
        std::string                   metric_    = "cosine";
        float                         threshold_ = 0.363F;
        cv::Size                      input_size_{320, 320};
        cv::Ptr<cv::FaceDetectorYN>   detector_;
        cv::Ptr<cv::FaceRecognizerSF> recognizer_;
    };

}  // namespace howdy::native
