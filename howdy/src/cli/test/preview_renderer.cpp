#include "cli/test/preview_renderer.hpp"

#include "config/runtime_config.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <string>
#include <utility>

#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

namespace {

	constexpr auto kWindowName = "Howdy Test";

}  // namespace

namespace howdy::native::test_cli_internal {

	TestPreviewRenderer::TestPreviewRenderer(howdy::native::VideoConfig                    config,
	                                         std::vector<howdy::native::EncodingModelInfo> models,
	                                         TestPreviewRendererDependencies dependencies)
	    : config_(std::move(config))
	    , models_(std::move(models))
	    , dependencies_(dependencies) {}

	void TestPreviewRenderer::Initialize() {
		if (initialized_) {
			return;
		}
		initialized_              = true;
		callback_cleanup_pending_ = true;
		window_cleanup_pending_   = true;
		try {
			if (dependencies_.initialize != nullptr) {
				dependencies_.initialize({.context = dependencies_.context, .renderer = this});
			} else {
				cv::namedWindow(kWindowName);
				cv::setMouseCallback(kWindowName, MouseCallback, this);
			}
		} catch (...) {
			try {
				Shutdown();
			} catch (...) {  // NOLINT(bugprone-empty-catch)
			}
			throw;
		}
	}

	void TestPreviewRenderer::Shutdown() {
		if (!initialized_) {
			return;
		}
		std::exception_ptr cleanup_error;
		auto attempt_cleanup = [&cleanup_error](bool &pending, auto &&operation) -> auto {
			if (!pending) {
				return;
			}
			try {
				operation();
				pending = false;
			} catch (...) {
				if (cleanup_error == nullptr) {
					cleanup_error = std::current_exception();
				}
			}
		};

		attempt_cleanup(callback_cleanup_pending_, [this] -> void {
			if (dependencies_.clear_callback != nullptr) {
				dependencies_.clear_callback(dependencies_.context);
			} else {
				cv::setMouseCallback(kWindowName, nullptr, nullptr);
			}
		});
		attempt_cleanup(window_cleanup_pending_, [this] -> void {
			if (dependencies_.destroy_window != nullptr) {
				dependencies_.destroy_window(dependencies_.context);
			} else {
				cv::destroyWindow(kWindowName);
			}
		});
		initialized_ = callback_cleanup_pending_ || window_cleanup_pending_;

		if (cleanup_error != nullptr) {
			std::rethrow_exception(cleanup_error);
		}
	}

	auto TestPreviewRenderer::Present(const howdy::native::PreviewFrameResult &frame_result,
	                                  const TestPreviewFrameStats             &stats) -> bool {
		const auto &gray_frame = frame_result.gray_frame;
		cv::Mat     overlay;
		cv::cvtColor(gray_frame.clone(), overlay, cv::COLOR_GRAY2BGR);
		const int height = gray_frame.rows;
		const int width  = gray_frame.cols;

		for (std::size_t index = 0; index < frame_result.brightness.bins_percent.size(); ++index) {
			const float     value_perc = frame_result.brightness.bins_percent[index];
			const int       bin_offset = 10 * static_cast<int>(index);
			const cv::Point p1(20 + bin_offset, 10);
			const cv::Point p2(10 + bin_offset, static_cast<int>((value_perc / 2.0F) + 10.0F));
			cv::rectangle(overlay, p1, p2, cv::Scalar(0, 200, 0), cv::FILLED);
		}

		PrintText(overlay, 0, height,
		          "RESOLUTION: " + std::to_string(height) + "x" + std::to_string(width));
		PrintText(overlay, 1, height, "FPS: " + std::to_string(stats.fps));
		PrintText(overlay, 2, height, "FRAMES: " + std::to_string(stats.total_frames));
		PrintText(overlay, 3, height,
		          "INFERENCE: " + std::to_string(frame_result.inference_time.count()) + "ms");
		PrintText(overlay, 4, height, "BACKEND: OpenCV YuNet/SFace");
		PrintText(overlay, 5, height,
		          std::string("CLAHE: ") + (config_.clahe_enabled ? "on" : "off"));

		if (slow_mode_) {
			cv::putText(overlay, "SLOW MODE", cv::Point(width - 66, height - 10),
			            cv::FONT_HERSHEY_SIMPLEX, 0.3, cv::Scalar(0, 0, 255), 0, cv::LINE_AA);
		}

		const bool dark_frame =
		    frame_result.status == howdy::native::PreviewFrameStatus::kBlackFrame ||
		    frame_result.status == howdy::native::PreviewFrameStatus::kTooDark;
		if (dark_frame) {
			cv::putText(overlay, "DARK FRAME", cv::Point(width - 68, 16), cv::FONT_HERSHEY_SIMPLEX,
			            0.3, cv::Scalar(0, 0, 255), 0, cv::LINE_AA);
		} else {
			cv::putText(overlay, "SCAN FRAME", cv::Point(width - 68, 16), cv::FONT_HERSHEY_SIMPLEX,
			            0.3, cv::Scalar(0, 255, 0), 0, cv::LINE_AA);
			for (const auto &face_result : frame_result.faces) {
				const auto &face = face_result.detection;
				cv::Scalar  color(0, 0, 230);
				const int   x = static_cast<int>(face.box.x);
				const int   y = static_cast<int>(face.box.y);
				const int   w = static_cast<int>(face.box.width);
				const int   h = static_cast<int>(face.box.height);

				if (face_result.status == howdy::native::PreviewFaceStatus::kEncodingFailed) {
					cv::putText(overlay, "encoding failed", cv::Point(x, std::max(0, y - 8)),
					            cv::FONT_HERSHEY_SIMPLEX, 0.3, color, 0, cv::LINE_AA);
				} else if (face_result.matching_attempted) {
					const auto &face_match = face_result.match;
					std::string face_text;
					if (!face_match.accepted) {
						face_text = "no match (" + cv::format("%.3f", face_match.score) + ")";
					} else {
						color             = cv::Scalar(0, 230, 0);
						const auto &model = models_[static_cast<std::size_t>(face_match.index)];
						face_text =
						    model.label + " (score: " + cv::format("%.3f", face_match.score) + ")";
					}

					cv::putText(overlay, face_text, cv::Point(x, std::max(0, y - 8)),
					            cv::FONT_HERSHEY_SIMPLEX, 0.3, color, 0, cv::LINE_AA);
				}

				cv::rectangle(overlay, cv::Rect(x, y, w, h), color, 2);
				cv::putText(overlay, cv::format("%.2f", face.confidence),
				            cv::Point(x, std::min(height - 4, y + h + 12)),
				            cv::FONT_HERSHEY_SIMPLEX, 0.3, color, 0, cv::LINE_AA);
				for (const auto &point : face.landmarks) {
					cv::circle(overlay,
					           cv::Point(static_cast<int>(point.x), static_cast<int>(point.y)), 2,
					           cv::Scalar(0, 255, 255), -1);
				}
			}
		}

		cv::Mat display_frame;
		cv::cvtColor(gray_frame, display_frame, cv::COLOR_GRAY2BGR);
		cv::addWeighted(overlay, 0.65, display_frame, 0.35, 0, display_frame);
		cv::imshow(kWindowName, display_frame);
		return cv::waitKey(1) == -1;
	}

	auto TestPreviewRenderer::SlowMode() const -> bool {
		return slow_mode_;
	}

	TestPreviewRendererCleanup::TestPreviewRendererCleanup(
	    std::optional<TestPreviewRenderer> &renderer)
	    : renderer_(renderer) {}

	TestPreviewRendererCleanup::~TestPreviewRendererCleanup() noexcept {
		try {
			Cleanup();
		} catch (...) {  // NOLINT(bugprone-empty-catch)
		}
	}

	void TestPreviewRendererCleanup::Cleanup() {
		if (cleanup_attempted_) {
			return;
		}
		cleanup_attempted_ = true;
		if (renderer_.has_value()) {
			renderer_->Shutdown();
		}
	}

	void ReplaceTestPreviewRenderer(std::optional<TestPreviewRenderer>           &renderer,
	                                howdy::native::VideoConfig                    config,
	                                std::vector<howdy::native::EncodingModelInfo> models,
	                                TestPreviewRendererDependencies               dependencies) {
		if (renderer.has_value()) {
			renderer->Shutdown();
		}
		renderer.emplace(std::move(config), std::move(models), dependencies);
	}

	// OpenCV MouseCallback requires int, int, int, int, void *.
	// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
	void TestPreviewRenderer::MouseCallback(int event, int x, int y, int flags, void *userdata) {
		(void)x;
		(void)y;
		(void)flags;
		auto *renderer = static_cast<TestPreviewRenderer *>(userdata);
		if (renderer != nullptr && event == cv::EVENT_LBUTTONDOWN) {
			renderer->slow_mode_ = !renderer->slow_mode_;
		}
	}

	void TestPreviewRenderer::PrintText(cv::Mat &overlay, int line_number, int height,
	                                    const std::string &text) {
		cv::putText(overlay, text, cv::Point(10, height - 10 - (10 * line_number)),
		            cv::FONT_HERSHEY_SIMPLEX, 0.3, cv::Scalar(0, 255, 0), 0, cv::LINE_AA);
	}

}  // namespace howdy::native::test_cli_internal
