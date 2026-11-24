#include "vision/face_detection.hpp"

#include <cmath>
#include <cstddef>
#include <format>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>

#include <opencv2/core/hal/interface.h>
#include <opencv2/core/mat.hpp>

namespace howdy::native {

	namespace {
		auto InvalidOutput(std::string message, const cv::Mat &rows) -> FaceDetectionResult {
			return FaceDetectionResult{
			    .status        = FaceDetectionStatus::kInvalidOutput,
			    .error_message = std::format("{} (dims={}, rows={}, cols={}, type={})", message,
				                             rows.dims, rows.rows, rows.cols, rows.type()),
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

		template <std::size_t index>
		[[nodiscard]] constexpr auto RowField(std::span<const float, 15> row) noexcept -> float {
			return row.subspan<index, 1>().front();
		}

		auto ValidateDetectionRow(std::span<const float, 15> row_values)
		    -> std::optional<std::string> {
			for (const float value : row_values) {
				if (!std::isfinite(value)) {
					return "YuNet output contains a non-finite value";
				}
			}
			if (RowField<2>(row_values) <= 0.0F || RowField<3>(row_values) <= 0.0F) {
				return "YuNet detection width and height must be greater than zero";
			}
			for (const float value : row_values.first<14>()) {
				if (!IntegerRenderingValueIsSafe(value)) {
					return "YuNet output contains geometry outside supported integer range";
				}
			}

			const int x      = static_cast<int>(RowField<0>(row_values));
			const int y      = static_cast<int>(RowField<1>(row_values));
			const int width  = static_cast<int>(RowField<2>(row_values));
			const int height = static_cast<int>(RowField<3>(row_values));
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

		auto MakeDetection(std::span<const float, 15> row_values) -> FaceDetection {
			FaceDetection detection{
			    .box        = cv::Rect2f(RowField<0>(row_values), RowField<1>(row_values),
				                         RowField<2>(row_values), RowField<3>(row_values)),
			    .confidence = RowField<14>(row_values),
			};
			for (auto &&[landmark, pair] : std::views::zip(
			         detection.landmarks, row_values.subspan<4, 10>() | std::views::chunk(2))) {
				landmark = cv::Point2f(pair.front(), pair.back());
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
			const std::span<const float, 15> row_values(rows.ptr<float>(row), 15);
			if (auto error = ValidateDetectionRow(row_values); error.has_value()) {
				return InvalidOutput(std::move(*error), rows);
			}
			result.detections.push_back(MakeDetection(row_values));
		}
		return result;
	}

}  // namespace howdy::native
