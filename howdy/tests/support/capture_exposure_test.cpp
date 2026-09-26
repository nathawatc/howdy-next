#include "support/capture_exposure.hpp"
#include "test_support.hpp"

#include <string>
#include <vector>

#include <opencv2/core/exception.hpp>
#include <opencv2/videoio.hpp>

namespace {

	// Test fixture exposes state for direct event assertions.
	// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
	struct Capture {
		std::vector<std::string> events;
		bool                     open_ok    = true;
		bool                     warm_up_ok = true;
		bool                     throw_auto = false;

		auto Open() -> bool {
			events.emplace_back("open");
			if (!open_ok) {
				return false;
			}
			// Production Open() performs its normal first warm-up before returning.
			events.emplace_back("open-warm-up");
			return true;
		}

		auto Set(int property, double value) -> bool {
			if (property == cv::CAP_PROP_AUTO_EXPOSURE) {
				events.emplace_back(value == 1.0 ? "auto" : "wrong-auto");
				if (throw_auto) {
					throw cv::Exception(cv::Error::StsError, "unsupported", "Set",
					                    "capture_exposure_test.cpp", 1);
				}
			} else {
				events.emplace_back(property == cv::CAP_PROP_EXPOSURE && value == 42.0
				                        ? "exposure"
				                        : "wrong-exposure");
			}
			return true;
		}

		auto WarmUp() -> bool {
			events.emplace_back("exposure-warm-up");
			return warm_up_ok;
		}

		auto Read() -> bool {
			events.emplace_back("read");
			return true;
		}
	};

	// NOLINTEND(misc-non-private-member-variables-in-classes)

}  // namespace

auto main() -> int {
	using howdy::test::Expect;
	bool ok = true;

	Capture automatic;
	ok &= Expect(howdy::native::OpenCaptureWithExposure(automatic, -1),
	             "automatic exposure opens capture");
	ok &= Expect(automatic.Read(), "automatic exposure starts real capture");
	ok &=
	    Expect(automatic.events == std::vector<std::string>{"open", "open-warm-up", "read"},
	           "automatic exposure keeps built-in warm-up without manual writes or extra warm-up");

	Capture manual;
	ok &=
	    Expect(howdy::native::OpenCaptureWithExposure(manual, 42), "manual exposure opens capture");
	ok &= Expect(manual.Read(), "manual exposure starts real capture");
	ok &= Expect(manual.events == std::vector<std::string>{"open", "open-warm-up", "auto",
	                                                       "exposure", "exposure-warm-up", "auto",
	                                                       "exposure", "read"},
	             "manual exposure reapplies after second grab and before real capture");

	Capture failed_open{.open_ok = false};
	ok &= Expect(!howdy::native::OpenCaptureWithExposure(failed_open, 42),
	             "failed open stops manual exposure");
	ok &= Expect(failed_open.events == std::vector<std::string>{"open"},
	             "failed open skips property changes and warm-up");

	Capture failed_warm_up{.warm_up_ok = false};
	ok &= Expect(!howdy::native::OpenCaptureWithExposure(failed_warm_up, 42),
	             "manual exposure warm-up failure reaches caller");
	ok &= Expect(failed_warm_up.events == std::vector<std::string>{"open", "open-warm-up", "auto",
	                                                               "exposure", "exposure-warm-up"},
	             "warm-up failure aborts before second exposure apply");

	Capture rejected_auto{.throw_auto = true};
	ok &= Expect(howdy::native::OpenCaptureWithExposure(rejected_auto, 42),
	             "unsupported auto-exposure property is best effort");
	ok &= Expect(rejected_auto.events == std::vector<std::string>{"open", "open-warm-up", "auto",
	                                                              "exposure", "exposure-warm-up",
	                                                              "auto", "exposure"},
	             "explicit exposure is attempted after each auto-exposure exception");

	return ok ? 0 : 1;
}
