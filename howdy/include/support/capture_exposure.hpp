#pragma once

#include <opencv2/core/exception.hpp>
#include <opencv2/videoio.hpp>

namespace howdy::native {

	template <typename SetProperty>
	void ApplyManualExposure(int exposure, SetProperty set_property) {
		if (exposure == -1) {
			return;
		}

		try {
			(void)set_property(cv::CAP_PROP_AUTO_EXPOSURE, 1.0);
		} catch (const cv::Exception &) {  // NOLINT(bugprone-empty-catch)
			// Continue with the explicit exposure if the camera rejects this property.
		}

		try {
			(void)set_property(cv::CAP_PROP_EXPOSURE, static_cast<double>(exposure));
		} catch (const cv::Exception &) {  // NOLINT(bugprone-empty-catch)
			// Exposure changes are best effort on unsupported or disconnected cameras.
		}
	}

	template <typename Capture>
	auto OpenCaptureWithExposure(Capture &capture, int exposure) -> bool {
		if (!capture.Open()) {
			return false;
		}
		if (exposure == -1) {
			return true;
		}
		auto set_property = [&capture](int property, double value) -> bool {
			return capture.Set(property, value);
		};
		ApplyManualExposure(exposure, set_property);
		if (!capture.WarmUp()) {
			return false;
		}
		ApplyManualExposure(exposure, set_property);
		return true;
	}

}  // namespace howdy::native
