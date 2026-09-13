#include "config/runtime_config_loader.hpp"

#include "config/config_schema.hpp"
#include "config/config_utils.hpp"
#include "config/test_hooks.hpp"
#include "config_reader/internal.hpp"
#include "config_validation/internal.hpp"
#include "runtime_config_loader/internal.hpp"
#include "support/atomic_files.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <utility>

namespace howdy::native {

	namespace {

		template <RuntimeConfigLoadStatus status>
		auto FailureResult(const std::filesystem::path &path, std::string error_message,
		                   int error_code = 0) -> RuntimeConfigLoadResult {
			static_assert(status != RuntimeConfigLoadStatus::kOk);
			return RuntimeConfigLoadResult{
			    .ok            = false,
			    .status        = status,
			    .path          = path,
			    .error_message = std::move(error_message),
			    .error_code    = error_code,
			};
		}

		auto SuccessResult(const std::filesystem::path &path, RuntimeConfig config)
		    -> RuntimeConfigLoadResult {
			return RuntimeConfigLoadResult{
			    .ok     = true,
			    .status = RuntimeConfigLoadStatus::kOk,
			    .path   = path,
			    .config = std::move(config),
			};
		}

		auto ReadCoreConfig(const ConfigReader &reader) -> CoreConfig {
			using enum config_schema::OptionId;
			return {
			    .detection_notice    = ReadRuntimeBool(reader, kCoreDetectionNotice),
			    .no_confirmation     = ReadRuntimeBool(reader, kCoreNoConfirmation),
			    .abort_if_ssh        = ReadRuntimeBool(reader, kCoreAbortIfSsh),
			    .abort_if_lid_closed = ReadRuntimeBool(reader, kCoreAbortIfLidClosed),
			    .disabled            = ReadRuntimeBool(reader, kCoreDisabled),
			};
		}

		auto ReadVideoConfig(const ConfigReader &reader) -> VideoConfig {
			using enum config_schema::OptionId;
			return {
			    .timeout              = ReadRuntimeInt(reader, kVideoTimeout),
			    .device_path          = ReadRuntimeString(reader, kVideoDevicePath),
			    .warn_no_device       = ReadRuntimeBool(reader, kVideoWarnNoDevice),
			    .max_height           = ReadRuntimeFloat(reader, kVideoMaxHeight),
			    .frame_width          = ReadRuntimeInt(reader, kVideoFrameWidth),
			    .frame_height         = ReadRuntimeInt(reader, kVideoFrameHeight),
			    .clahe_enabled        = ReadRuntimeBool(reader, kVideoClaheEnabled),
			    .clahe_clip_limit     = ReadRuntimeFloat(reader, kVideoClaheClipLimit),
			    .clahe_tile_grid_size = ReadRuntimeInt(reader, kVideoClaheTileGridSize),
			    .dark_threshold       = ReadRuntimeFloat(reader, kVideoDarkThreshold),
			    .force_mjpeg          = ReadRuntimeBool(reader, kVideoForceMjpeg),
			    .exposure             = ReadRuntimeInt(reader, kVideoExposure),
			    .device_fps           = ReadRuntimeInt(reader, kVideoDeviceFps),
			    .rotate               = ReadRuntimeInt(reader, kVideoRotate),
			};
		}

		auto ReadFaceConfig(const ConfigReader &reader, FaceMetric metric) -> FaceConfig {
			using enum config_schema::OptionId;
			FaceConfig config{
			    .yunet_score_threshold = ReadRuntimeFloat(reader, kFaceYunetScoreThreshold),
			    .yunet_nms_threshold   = ReadRuntimeFloat(reader, kFaceYunetNmsThreshold),
			    .yunet_top_k           = ReadRuntimeInt(reader, kFaceYunetTopK),
			    .sface_metric          = metric,
			};
			config.sface_threshold = ReadSfaceThreshold(reader, metric);
			return config;
		}

		auto ReadDebugConfig(const ConfigReader &reader) -> DebugConfig {
			return {.end_report =
			            ReadRuntimeBool(reader, config_schema::OptionId::kDebugEndReport)};
		}

		auto PopulateRuntimeConfig(const ConfigReader &reader) -> std::optional<RuntimeConfig> {
			const auto metric = ReadSfaceMetric(reader);
			if (!metric.has_value()) {
				return std::nullopt;
			}

			RuntimeConfig config;
			config.core  = ReadCoreConfig(reader);
			config.video = ReadVideoConfig(reader);
			config.face  = ReadFaceConfig(reader, *metric);
			config.debug = ReadDebugConfig(reader);
			return config;
		}

	}  // namespace

	auto LoadRuntimeConfig(const std::filesystem::path                  &config_path,
	                       const std::optional<uid_t>                    owner_uid,
	                       const file_security_internal::ValidationRoot &validation_root)
	    -> RuntimeConfigLoadResult {
		const int opened_fd =
		    open(config_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
		if (opened_fd < 0) {
			const int open_error = errno;
			return FailureResult<RuntimeConfigLoadStatus::kPathError>(
			    config_path,
			    "Failed to inspect Config file: " + config_path.string() + " (" +
			        std::strerror(open_error) + ")" + ConfigAccessErrorHint(open_error),
			    open_error);
		}
		ScopedFd fd(opened_fd);
		if (config_test_hooks::Current()) {
			config_test_hooks::Current()();
		}

		const auto security =
		    CheckSecureConfigFd(fd.Get(), config_path, owner_uid, validation_root);
		if (!security.ok) {
			return FailureResult<RuntimeConfigLoadStatus::kPathError>(
			    config_path, security.error_message, security.error_code);
		}

		const auto content = ReadConfigFromFd(fd.Get(), std::nullopt);
		fd.Reset();
		if (!content.has_value()) {
			return FailureResult<RuntimeConfigLoadStatus::kParseError>(
			    config_path, "Failed to parse config: " + config_path.string() + " (error -1)");
		}

		const ConfigReader reader(config_path.string(), *content);
		if (!reader.Ok()) {
			return FailureResult<RuntimeConfigLoadStatus::kParseError>(
			    config_path, "Failed to parse config: " + config_path.string() + " (error " +
			                     std::to_string(reader.ParseError()) + ")");
		}

		if (const auto validation = ValidateRuntimeConfig(reader)) {
			return FailureResult<RuntimeConfigLoadStatus::kInvalidRuntimeValue>(
			    config_path,
			    "Invalid runtime config in " + config_path.string() + ": " + *validation);
		}

		auto populated = PopulateRuntimeConfig(reader);
		if (!populated.has_value()) {
			return FailureResult<RuntimeConfigLoadStatus::kInvalidRuntimeValue>(
			    config_path,
			    "Invalid runtime config in " + config_path.string() + ": unknown face metric");
		}
		return SuccessResult(config_path, std::move(*populated));
	}
}  // namespace howdy::native
