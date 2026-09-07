#include "vision/face_detection.hpp"

#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <utility>

#include <opencv2/core.hpp>

namespace howdy::native {

	namespace {
		auto InvalidOutput(std::string message, const cv::Mat &rows) -> FaceDetectionResult {
			message += " (dims=" + std::to_string(rows.dims) +
			           ", rows=" + std::to_string(rows.rows) +
			           ", cols=" + std::to_string(rows.cols) +
			           ", type=" + std::to_string(rows.type()) + ")";
			return FaceDetectionResult{
			    .status        = FaceDetectionStatus::kInvalidOutput,
			    .error_message = std::move(message),
			};
		}

		auto IntegerRenderingValueIsSafe(float value) -> bool {
			const double truncated = std::trunc(static_cast<double>(value));
			return truncated >= static_cast<double>(std::numeric_limits<int>::min()) &&
			       truncated <= static_cast<double>(std::numeric_limits<int>::max());
		}

		auto IntegerRenderingSumIsSafe(double value) -> bool {
			return value >= static_cast<double>(std::numeric_limits<int>::min()) &&
			       value <= static_cast<double>(std::numeric_limits<int>::max());
		}

		auto ValidateDetectionRow(const cv::Mat &rows, int row) -> std::optional<std::string> {
			for (int column = 0; column < rows.cols; ++column) {
				if (!std::isfinite(rows.at<float>(row, column))) {
					return "YuNet output contains a non-finite value";
				}
			}
			if (rows.at<float>(row, 2) <= 0.0F || rows.at<float>(row, 3) <= 0.0F) {
				return "YuNet detection width and height must be greater than zero";
			}
			for (int column = 0; column < 14; ++column) {
				if (!IntegerRenderingValueIsSafe(rows.at<float>(row, column))) {
					return "YuNet output contains geometry outside supported integer range";
				}
			}

			const int x      = static_cast<int>(rows.at<float>(row, 0));
			const int y      = static_cast<int>(rows.at<float>(row, 1));
			const int width  = static_cast<int>(rows.at<float>(row, 2));
			const int height = static_cast<int>(rows.at<float>(row, 3));
			if (width < 1 || height < 1) {
				return "YuNet detection width and height must render as at least one pixel";
			}
			if (!IntegerRenderingSumIsSafe(static_cast<double>(x) + width - 1.0) ||
			    !IntegerRenderingSumIsSafe(static_cast<double>(y) + height - 1.0) ||
			    !IntegerRenderingSumIsSafe(static_cast<double>(y) + height + 12.0)) {
				return "YuNet output box geometry overflows preview rendering";
			}
			return std::nullopt;
		}

		auto MakeDetection(const cv::Mat &rows, int row) -> FaceDetection {
			FaceDetection detection{
			    .box        = cv::Rect2f(rows.at<float>(row, 0), rows.at<float>(row, 1),
			                             rows.at<float>(row, 2), rows.at<float>(row, 3)),
			    .confidence = rows.at<float>(row, 14),
			};
			for (std::size_t index = 0; index < detection.landmarks.size(); ++index) {
				const int column = 4 + static_cast<int>(index * 2);
				detection.landmarks[index] =
				    cv::Point2f(rows.at<float>(row, column), rows.at<float>(row, column + 1));
			}
			return detection;
		}
	}  // namespace

	auto ParseYunetDetections(const cv::Mat &rows) -> FaceDetectionResult {
		if (rows.dims == 0) {
			return {};
		}
		if (rows.dims != 2) {
			return InvalidOutput("YuNet output must be two-dimensional", rows);
		}
		if (rows.type() != CV_32FC1) {
			return InvalidOutput("YuNet output must contain single-channel 32-bit floats", rows);
		}
		if (rows.rows == 0 && rows.cols == 0) {
			return {};
		}
		if (rows.cols != 15) {
			return InvalidOutput("YuNet output must have exactly 15 columns", rows);
		}
		if (rows.rows == 0) {
			return {};
		}

		FaceDetectionResult result;
		result.detections.reserve(static_cast<std::size_t>(rows.rows));
		for (int row = 0; row < rows.rows; ++row) {
			if (auto error = ValidateDetectionRow(rows, row); error.has_value()) {
				return InvalidOutput(std::move(*error), rows);
			}
			result.detections.push_back(MakeDetection(rows, row));
		}
		return result;
	}

}  // namespace howdy::native
