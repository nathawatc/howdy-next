#include "cli/test.hpp"

#include "app/command_invocation.hpp"
#include "cli/privilege_drop.hpp"
#include "cli/test/gui_probe.hpp"
#include "cli/test/internal.hpp"
#include "cli/test/preview_renderer.hpp"
#include "cli/test/preview_session.hpp"
#include "config/runtime_config.hpp"
#include "config/runtime_config_loader.hpp"
#include "storage/user_model_status.hpp"
#include "storage/user_model_store.hpp"
#include "storage/user_model_types.hpp"
#include "support/capture_exposure.hpp"
#include "support/invoking_user.hpp"
#include "support/invoking_user_env.hpp"
#include "vision/face_model.hpp"
#include "vision/preview_engine.hpp"
#include "vision/video_capture.hpp"

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <grp.h>
#include <unistd.h>

#include <opencv2/core/mat.hpp>

#include <sys/types.h>

namespace {

	namespace test_cli_internal = howdy::native::test_cli_internal;

	struct TestProductionContext {
		howdy::native::UserModelStore                         store;
		std::optional<howdy::native::FaceModel>               face_model;
		std::optional<howdy::native::VideoCapture>            capture;
		std::optional<test_cli_internal::TestPreviewRenderer> renderer;
		howdy::native::UserModelLoadResult                    loaded_models;
		cv::Mat                                               prefetched_gray_frame;
		int                                                   exposure = -1;
	};

	auto DropToInvokingGuiUser() -> bool {
		if (geteuid() != 0) {
			return true;
		}

		const auto invoking_identity = howdy::native::ResolveInvokingIdentity();
		if (invoking_identity.status != howdy::native::InvokingIdentityStatus::kResolved ||
		    !invoking_identity.user.has_value()) {
			return false;
		}

		const auto &invoking_user = *invoking_identity.user;
		if (!howdy::native::cli_internal::DropPrivilegesPermanently(
		        invoking_user, howdy::native::cli_internal::SystemPrivilegeDropOperations())) {
			return false;
		}
		howdy::native::ResetInvokingUserGuiEnvironment(invoking_user);
		return true;
	}

	auto GetenvStringView(const char *name) -> std::string_view {
		const char *value = std::getenv(name);
		if (value == nullptr) {
			return {};
		}
		return value;
	}

	auto ProductionGuiProbeOperations() -> test_cli_internal::GuiProbeOperations {
		return {
		    .context   = nullptr,
		    .getgroups = [](void * /*context*/, int size, gid_t *groups) -> int {
			    return ::getgroups(size, groups);
		    },
		    .setgroups = [](void * /*context*/, std::size_t count, const gid_t *groups) -> int {
			    return ::setgroups(count, groups);
		    },
		    .initgroups = [](void * /*context*/, const char *user, gid_t gid) -> int {
			    return ::initgroups(user, gid);
		    },
		    .getegid = [](void * /*context*/) -> gid_t {
			    return ::getegid();
		    },
		    .geteuid = [](void * /*context*/) -> uid_t {
			    return ::geteuid();
		    },
		    .setegid = [](void * /*context*/, gid_t gid) -> int {
			    return ::setegid(gid);
		    },
		    .seteuid = [](void * /*context*/, uid_t uid) -> int {
			    return ::seteuid(uid);
		    },
		    .probe = [](void * /*context*/, const howdy::native::InvokingUser &user) -> void {
			    howdy::native::PrepareInvokingUserGuiEnvironment(user);
		    },
		};
	}

	// The display check runs before the permanent drop because the camera must still open
	// as root, so the ~/.Xauthority and /run/user/<uid> probe cannot simply move after
	// setuid. Probing under the invoking user's effective IDs keeps root from following
	// user-controlled symlinks, while the retained saved UID 0 lets preflight continue as
	// root until DropToInvokingGuiUser. Returns an error message on failure.
	auto PrepareInvokingGuiEnvironment() -> std::optional<std::string> {
		if (geteuid() != 0) {
			return std::nullopt;
		}

		const auto invoking_identity = howdy::native::ResolveInvokingIdentity();
		if (invoking_identity.status != howdy::native::InvokingIdentityStatus::kResolved ||
		    !invoking_identity.user.has_value()) {
			return std::nullopt;
		}
		return test_cli_internal::RunGuiProbeAsInvokingUser(*invoking_identity.user,
		                                                    ProductionGuiProbeOperations());
	}

	auto HasGraphicalDisplayEnvironment() -> bool {
		return test_cli_internal::HasGraphicalDisplayEnvironment(
		    GetenvStringView("DISPLAY"),
		    GetenvStringView(howdy::native::kWaylandDisplayEnvironmentVariable),
		    GetenvStringView(howdy::native::kXdgRuntimeDirEnvironmentVariable));
	}

	auto TestCliLoadRuntimeConfigDependency([[maybe_unused]] void *context)
	    -> howdy::native::RuntimeConfigLoadResult {
		return howdy::native::LoadRuntimeConfig();
	}

	auto FaceModelReadyDependency(void *context, const howdy::native::RuntimeConfig &config,
	                              const std::string &user)
	    -> test_cli_internal::TestPreflightOperationResult {
		auto *production_context = static_cast<TestProductionContext *>(context);
		if (production_context == nullptr) {
			return test_cli_internal::TestPreflightOperationResult{
			    .error_message = howdy::native::kFaceModelNotInitializedMessage,
			};
		}

		auto const &face_model = production_context->face_model.emplace(config.face);
		if (!face_model.Ok()) {
			return test_cli_internal::TestPreflightOperationResult{
			    .error_message = face_model.ErrorMessage(),
			};
		}

		production_context->loaded_models = howdy::native::UserModelLoadResult{};
		if (!user.empty()) {
			production_context->loaded_models =
			    production_context->store.LoadModels(user, howdy::native::FaceModel::kBackendName);
			const auto status  = production_context->loaded_models.status;
			const auto outcome = howdy::native::Classify(status);
			if (status == howdy::native::UserModelStatus::kIncompatibleBackend) {
				std::cout << "Warning: Stored face models use an incompatible backend; matching "
				             "disabled\n";
			} else if (status == howdy::native::UserModelStatus::kInvalidUser) {
				std::cout << "Warning: Invalid user name; matching disabled\n";
			} else if (outcome == howdy::native::UserModelOutcome::kAbsent) {
				std::cout
				    << "Warning: No face model found for this user, detection will run without "
				       "matching\n";
			} else if (outcome == howdy::native::UserModelOutcome::kInsecure) {
				std::cout
				    << "Warning: Stored face model path is insecure, detection will run without "
				       "matching\n";
			} else if (outcome != howdy::native::UserModelOutcome::kOk) {
				std::cout
				    << "Warning: Failed to read stored face models, detection will run without "
				       "matching\n";
			}
		}

		test_cli_internal::ReplaceTestPreviewRenderer(
		    production_context->renderer, config.video,
		    production_context->loaded_models.stored.models);
		return test_cli_internal::TestPreflightOperationResult{.ok = true};
	}

	auto HasGraphicalDisplayDependency([[maybe_unused]] void *context) -> bool {
		return HasGraphicalDisplayEnvironment();
	}

	auto OpenCameraDependency(void *context, const howdy::native::RuntimeConfig &config,
	                          const std::string &device_path)
	    -> test_cli_internal::TestPreflightOperationResult {
		auto *production_context = static_cast<TestProductionContext *>(context);
		if (production_context == nullptr) {
			return test_cli_internal::TestPreflightOperationResult{
			    .error_message = "Camera was not initialized",
			};
		}

		auto settings        = howdy::native::LoadCaptureSettings(config.video);
		settings.device_path = device_path;
		auto &capture        = production_context->capture.emplace(settings);
		if (!capture.Open()) {
			return test_cli_internal::TestPreflightOperationResult{
			    .error_message = capture.ErrorMessage(),
			};
		}
		return test_cli_internal::TestPreflightOperationResult{.ok = true};
	}

	auto ReadCameraDependency(void *context) -> bool {
		auto *production_context = static_cast<TestProductionContext *>(context);
		if (production_context == nullptr || !production_context->capture.has_value()) {
			return false;
		}
		cv::Mat frame;
		return production_context->capture->Read(frame, &production_context->prefetched_gray_frame);
	}

	auto ReadPreviewGrayFrameDependency(void *context, cv::Mat &gray_frame) -> bool {
		auto *production_context = static_cast<TestProductionContext *>(context);
		if (production_context == nullptr || !production_context->capture.has_value()) {
			return false;
		}
		cv::Mat frame;
		return production_context->capture->Read(frame, &gray_frame);
	}

	void RestorePreviewExposureDependency(void *context) {
		auto *production_context = static_cast<TestProductionContext *>(context);
		if (production_context == nullptr || !production_context->capture.has_value()) {
			return;
		}
		howdy::native::ApplyManualExposure(
		    production_context->exposure, [production_context](int property, double value) -> bool {
			    return production_context->capture->Set(property, value);
		    });
	}

	auto SwitchGuiUserDependency([[maybe_unused]] void *context) -> bool {
		return DropToInvokingGuiUser();
	}

	void InitializeGuiDependency(void *context) {
		auto *production_context = static_cast<TestProductionContext *>(context);
		if (production_context != nullptr && production_context->renderer.has_value()) {
			production_context->renderer->Initialize();
		}
	}

	auto PresentPreviewFrameDependency(void                                           *context,
	                                   const howdy::native::PreviewFrameResult        &frame_result,
	                                   const test_cli_internal::TestPreviewFrameStats &stats)
	    -> bool {
		return static_cast<test_cli_internal::TestPreviewRenderer *>(context)->Present(frame_result,
		                                                                               stats);
	}

	auto PreviewSlowModeDependency(void *context) -> bool {
		return static_cast<test_cli_internal::TestPreviewRenderer *>(context)->SlowMode();
	}

	auto PreviewNowDependency([[maybe_unused]] void *context)
	    -> std::chrono::steady_clock::time_point {
		return std::chrono::steady_clock::now();
	}

	void PreviewSleepDependency([[maybe_unused]] void    *context,
	                            std::chrono::milliseconds duration) {
		std::this_thread::sleep_for(duration);
	}

	auto RunPreviewDependency(void *context, const howdy::native::RuntimeConfig &config,
	                          const std::string &user, const std::string &device_path)
	    -> test_cli_internal::TestPreviewResult {
		auto *production_context = static_cast<TestProductionContext *>(context);
		if (production_context == nullptr) {
			return test_cli_internal::TestPreviewResult{
			    .status        = test_cli_internal::TestPreviewStatus::kFaceModelError,
			    .error_message = howdy::native::kFaceModelNotInitializedMessage,
			};
		}
		test_cli_internal::PreviewCleanup const cleanup{production_context->renderer,
		                                                production_context->capture};

		if (auto probe_error = PrepareInvokingGuiEnvironment()) {
			return test_cli_internal::TestPreviewResult{
			    .status        = test_cli_internal::TestPreviewStatus::kGuiUserError,
			    .error_message = std::move(*probe_error),
			};
		}

		auto preflight_result = test_cli_internal::RunPreviewPreflight(
		    config, user,
		    test_cli_internal::TestPreviewPreflightDependencies{
		        .context               = production_context,
		        .face_model_ready      = FaceModelReadyDependency,
		        .has_graphical_display = HasGraphicalDisplayDependency,
		        .open_camera           = OpenCameraDependency,
		        .read_camera           = ReadCameraDependency,
		        .switch_gui_user       = SwitchGuiUserDependency,
		        .initialize_gui        = InitializeGuiDependency,
		    },
		    device_path);
		if (preflight_result.status != test_cli_internal::TestPreviewStatus::kOk) {
			return preflight_result;
		}
		if (!production_context->face_model.has_value() ||
		    !production_context->capture.has_value() || !production_context->renderer.has_value()) {
			return {
			    .status        = test_cli_internal::TestPreviewStatus::kFaceModelError,
			    .error_message = "Test preview was not initialized",
			};
		}

		production_context->exposure               = config.video.exposure;
		auto                        &face_model    = *production_context->face_model;
		auto                        &loaded_models = production_context->loaded_models;
		howdy::native::PreviewEngine preview_engine(
		    config.video,
		    {
		        .inference = howdy::native::FaceModelInferenceOperations(face_model),
		    },
		    howdy::native::PreparedFaceMatcher(std::move(loaded_models.stored.encodings),
			                                   config.face.sface_metric,
			                                   config.face.sface_threshold),
		    loaded_models.stored.models.size(),
		    loaded_models.status == howdy::native::UserModelStatus::kOk);

		test_cli_internal::TestPreviewSession preview_session(
		    config.video, preview_engine,
		    {
		        .capture_context  = production_context,
		        .read_gray_frame  = ReadPreviewGrayFrameDependency,
		        .restore_exposure = RestorePreviewExposureDependency,
		        .renderer_context = &*production_context->renderer,
		        .present          = PresentPreviewFrameDependency,
		        .slow_mode        = PreviewSlowModeDependency,
		        .clock_context    = nullptr,
		        .now              = PreviewNowDependency,
		        .sleep_context    = nullptr,
		        .sleep            = PreviewSleepDependency,
		    });
		return test_cli_internal::RunPreviewSessionWithRetainedFrame(
		    preview_session, production_context->prefetched_gray_frame);
	}

}  // namespace

auto TestMain(const howdy::native::CommandInvocation &invocation) -> int {
	TestProductionContext production_context{.store = howdy::native::UserModelStore::Production()};
	return howdy::native::test_cli_internal::TestMainWithDependencies(
	    invocation, howdy::native::test_cli_internal::TestDependencies{
	                    .context             = &production_context,
	                    .load_runtime_config = TestCliLoadRuntimeConfigDependency,
	                    .run_preview         = RunPreviewDependency,
	                });
}
