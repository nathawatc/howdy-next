#include "compare/logic.hpp"

#include "protocol/compare_exit.hpp"

#include <exception>
#include <ostream>
#include <string_view>

#include <opencv2/core/exception.hpp>

namespace howdy::native {

	auto TimeoutExit(int dark_tries, int valid_frames) -> CompareExit {
		if (dark_tries > 0 && valid_frames == dark_tries) {
			return CompareExit::kTooDark;
		}
		return CompareExit::kTimeoutReached;
	}

	auto CompareAbortFromCvException(const cv::Exception &error, std::ostream &stream,
	                                 std::string_view context) -> CompareExit {
		stream << "OpenCV exception during " << context << ": " << error.what() << "\n";
		return CompareExit::kAbort;
	}

	auto CompareAbortFromException(const std::exception &error, std::ostream &stream,
	                               std::string_view context) -> CompareExit {
		stream << "Unhandled exception during " << context << ": " << error.what() << "\n";
		return CompareExit::kAbort;
	}

	auto CompareAbortFromUnknownException(std::ostream &stream, std::string_view context)
	    -> CompareExit {
		stream << "Unknown exception during " << context << "\n";
		return CompareExit::kAbort;
	}

}  // namespace howdy::native
