#include "test_support.hpp"
#include "vision/face_detection.hpp"

#include <array>
#include <limits>
#include <string>

namespace {

	using howdy::test::expect;

	auto ValidRows(int count = 1) -> cv::Mat {
		cv::Mat rows(count, 15, CV_32FC1);
		for (int row = 0; row < count; ++row) {
			for (int column = 0; column < 15; ++column) {
				rows.at<float>(row, column) = static_cast<float>((row * 100) + column + 1);
			}
		}
		return rows;
	}

	auto Rejected(const cv::Mat &rows, const std::string &name) -> bool {
		const auto result = howdy::native::ParseYunetDetections(rows);
		bool       ok     = true;
		ok &= expect(result.status == howdy::native::FaceDetectionStatus::kInvalidOutput,
		             name + " is rejected");
		ok &= expect(!result.error_message.empty(), name + " has diagnostic");
		ok &= expect(result.error_message.contains("dims="), name + " diagnostic includes dims");
		ok &= expect(result.error_message.contains("rows="), name + " diagnostic includes rows");
		ok &= expect(result.error_message.contains("cols="), name + " diagnostic includes cols");
		ok &= expect(result.error_message.contains("type="), name + " diagnostic includes type");
		ok &= expect(result.detections.empty(), name + " emits no detections");
		return ok;
	}

}  // namespace

auto main() -> int {
	bool ok = true;

	const auto empty = howdy::native::ParseYunetDetections(cv::Mat{});
	ok &=
	    expect(empty.Ok() && empty.detections.empty(), "empty output succeeds without detections");
	const auto canonical_typed_empty = howdy::native::ParseYunetDetections(cv::Mat(0, 0, CV_32FC1));
	ok &= expect(canonical_typed_empty.Ok() && canonical_typed_empty.detections.empty(),
	             "canonical typed empty output succeeds without detections");
	const auto zero_rows = howdy::native::ParseYunetDetections(cv::Mat(0, 15, CV_32FC1));
	ok &= expect(zero_rows.Ok() && zero_rows.detections.empty(),
	             "valid zero-row output succeeds without detections");

	const auto one = howdy::native::ParseYunetDetections(ValidRows());
	ok &= expect(one.Ok() && one.detections.size() == 1, "valid row succeeds");
	if (one.detections.size() == 1) {
		const auto &face = one.detections.front();
		ok &= expect(face.box == cv::Rect2f(1.0F, 2.0F, 3.0F, 4.0F), "box maps exactly");
		for (std::size_t index = 0; index < face.landmarks.size(); ++index) {
			const auto value = static_cast<float>(5 + (index * 2));
			ok &= expect(face.landmarks[index] == cv::Point2f(value, value + 1.0F),
			             "landmark maps exactly");
		}
		ok &= expect(face.confidence == 15.0F, "confidence maps exactly");
	}
	auto fractional_row            = ValidRows();
	fractional_row.at<float>(0, 0) = 1.75F;
	fractional_row.at<float>(0, 1) = 2.75F;
	fractional_row.at<float>(0, 2) = 1.9F;
	fractional_row.at<float>(0, 3) = 1.2F;
	const auto fractional          = howdy::native::ParseYunetDetections(fractional_row);
	ok &= expect(fractional.Ok() && fractional.detections.size() == 1,
	             "renderable fractional box succeeds");
	if (fractional.detections.size() == 1) {
		ok &= expect(fractional.detections.front().box == cv::Rect2f(1.75F, 2.75F, 1.9F, 1.2F),
		             "renderable fractional box is preserved");
	}

	const auto multiple = howdy::native::ParseYunetDetections(ValidRows(2));
	ok &= expect(multiple.Ok() && multiple.detections.size() == 2, "multiple rows succeed");
	if (multiple.detections.size() == 2) {
		ok &= expect(multiple.detections[0].box.x == 1.0F && multiple.detections[1].box.x == 101.0F,
		             "row order is preserved");
	}

	const std::array dimensions = {1, 1, 15};
	ok &= Rejected(cv::Mat(3, dimensions.data(), CV_32FC1, cv::Scalar(1.0F)), "wrong rank");
	ok &= Rejected(cv::Mat(1, 15, CV_64FC1, cv::Scalar(1.0)), "wrong type");
	ok &= Rejected(cv::Mat(1, 14, CV_32FC1, cv::Scalar(1.0F)), "wrong column count");
	ok &= Rejected(cv::Mat(0, 14, CV_32FC1), "zero-row wrong column count");
	ok &= Rejected(cv::Mat(0, 15, CV_64FC1), "zero-row wrong type");

	auto nan            = ValidRows();
	nan.at<float>(0, 7) = std::numeric_limits<float>::quiet_NaN();
	ok &= Rejected(nan, "NaN");
	auto infinity             = ValidRows();
	infinity.at<float>(0, 14) = std::numeric_limits<float>::infinity();
	ok &= Rejected(infinity, "infinity");
	auto zero_width            = ValidRows();
	zero_width.at<float>(0, 2) = 0.0F;
	ok &= Rejected(zero_width, "zero width");
	auto negative_height            = ValidRows();
	negative_height.at<float>(0, 3) = -1.0F;
	ok &= Rejected(negative_height, "negative height");
	auto maximum_box_x            = ValidRows();
	maximum_box_x.at<float>(0, 0) = std::numeric_limits<float>::max();
	ok &= Rejected(maximum_box_x, "FLT_MAX box x");
	auto maximum_landmark_x            = ValidRows();
	maximum_landmark_x.at<float>(0, 4) = std::numeric_limits<float>::max();
	ok &= Rejected(maximum_landmark_x, "FLT_MAX landmark x");
	auto right_edge_overflow            = ValidRows();
	right_edge_overflow.at<float>(0, 0) = 2'147'483'520.0F;
	right_edge_overflow.at<float>(0, 2) = 256.0F;
	ok &= Rejected(right_edge_overflow, "box right edge overflow");
	auto text_position_overflow            = ValidRows();
	text_position_overflow.at<float>(0, 1) = 2'147'483'520.0F;
	text_position_overflow.at<float>(0, 3) = 116.0F;
	ok &= Rejected(text_position_overflow, "text position overflow");
	auto subpixel_width_at_int_min            = ValidRows();
	subpixel_width_at_int_min.at<float>(0, 0) = static_cast<float>(std::numeric_limits<int>::min());
	subpixel_width_at_int_min.at<float>(0, 2) = 0.5F;
	ok &= Rejected(subpixel_width_at_int_min, "subpixel width at INT_MIN x");
	auto subpixel_height_at_int_min = ValidRows();
	subpixel_height_at_int_min.at<float>(0, 1) =
	    static_cast<float>(std::numeric_limits<int>::min());
	subpixel_height_at_int_min.at<float>(0, 3) = 0.5F;
	ok &= Rejected(subpixel_height_at_int_min, "subpixel height at INT_MIN y");

	return ok ? 0 : 1;
}
