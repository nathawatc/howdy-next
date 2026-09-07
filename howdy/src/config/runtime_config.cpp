#include "config/runtime_config.hpp"

#include "config/config_reader.hpp"
#include "config/config_schema.hpp"
#include "config/config_utils.hpp"
#include "config/config_validation.hpp"
#include "config/config_values.hpp"
#include "config/test_hooks.hpp"
#include "support/atomic_files.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <utility>

namespace howdy::native {
	namespace config_test_hooks {

		auto Current() -> AfterOpenBeforeRead & {
			static AfterOpenBeforeRead hook;
			return hook;
		}

		ScopedHooks::ScopedHooks(AfterOpenBeforeRead hook)
		    : previous_(std::move(Current())) {
			Current() = std::move(hook);
		}

		ScopedHooks::~ScopedHooks() {
			Current() = std::move(previous_);
		}

	}  // namespace config_test_hooks

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

		struct RuntimeValueSource {
			const ConfigReader *reader = nullptr;

			[[nodiscard]] auto ReadBool(config_schema::OptionId id) const -> bool {
				return reader == nullptr ? DefaultBool(id) : ReadRuntimeBool(*reader, id);
			}

			[[nodiscard]] auto ReadInt(config_schema::OptionId id) const -> int {
				return reader == nullptr ? DefaultInt(id) : ReadRuntimeInt(*reader, id);
			}

			[[nodiscard]] auto ReadFloat(config_schema::OptionId id) const -> float {
				return reader == nullptr ? DefaultFloat(id) : ReadRuntimeFloat(*reader, id);
			}

			[[nodiscard]] auto ReadString(config_schema::OptionId id) const -> std::string {
				return reader == nullptr ? DefaultString(id) : ReadRuntimeString(*reader, id);
			}

			[[nodiscard]] auto ReadFaceMetric() const -> std::optional<FaceMetric> {
				return reader == nullptr
				           ? std::optional<FaceMetric>(config_schema::kSfaceDefaultMetric)
				           : ReadSfaceMetric(*reader);
			}

			[[nodiscard]] auto ReadSfaceThreshold(config_schema::OptionId id,
			                                      FaceMetric              metric) const -> float {
				return reader == nullptr ? DefaultFloat(id)
				                         : howdy::native::ReadSfaceThreshold(*reader, metric);
			}

		private:
			static auto DefaultBool(config_schema::OptionId id) -> bool {
				return config_schema::RuntimeDefaultBool(id);
			}

			static auto DefaultInt(config_schema::OptionId id) -> int {
				return config_schema::RuntimeDefaultInt(id);
			}

			static auto DefaultFloat(config_schema::OptionId id) -> float {
				return config_schema::RuntimeDefaultFloat(id);
			}

			static auto DefaultString(config_schema::OptionId id) -> std::string {
				return std::string(config_schema::RuntimeDefaultString(id));
			}
		};

		auto ReadCoreConfig(const RuntimeValueSource &source) -> CoreConfig {
			using enum config_schema::OptionId;
			return {
			    .detection_notice    = source.ReadBool(kCoreDetectionNotice),
			    .no_confirmation     = source.ReadBool(kCoreNoConfirmation),
			    .abort_if_ssh        = source.ReadBool(kCoreAbortIfSsh),
			    .abort_if_lid_closed = source.ReadBool(kCoreAbortIfLidClosed),
			    .disabled            = source.ReadBool(kCoreDisabled),
			};
		}

		auto ReadVideoConfig(const RuntimeValueSource &source) -> VideoConfig {
			using enum config_schema::OptionId;
			return {
			    .timeout              = source.ReadInt(kVideoTimeout),
			    .device_path          = source.ReadString(kVideoDevicePath),
			    .warn_no_device       = source.ReadBool(kVideoWarnNoDevice),
			    .max_height           = source.ReadFloat(kVideoMaxHeight),
			    .frame_width          = source.ReadInt(kVideoFrameWidth),
			    .frame_height         = source.ReadInt(kVideoFrameHeight),
			    .clahe_enabled        = source.ReadBool(kVideoClaheEnabled),
			    .clahe_clip_limit     = source.ReadFloat(kVideoClaheClipLimit),
			    .clahe_tile_grid_size = source.ReadInt(kVideoClaheTileGridSize),
			    .dark_threshold       = source.ReadFloat(kVideoDarkThreshold),
			    .force_mjpeg          = source.ReadBool(kVideoForceMjpeg),
			    .exposure             = source.ReadInt(kVideoExposure),
			    .device_fps           = source.ReadInt(kVideoDeviceFps),
			    .rotate               = source.ReadInt(kVideoRotate),
			};
		}

		auto ReadFaceConfig(const RuntimeValueSource &source, FaceMetric metric) -> FaceConfig {
			using enum config_schema::OptionId;
			FaceConfig config{
			    .yunet_score_threshold = source.ReadFloat(kFaceYunetScoreThreshold),
			    .yunet_nms_threshold   = source.ReadFloat(kFaceYunetNmsThreshold),
			    .yunet_top_k           = source.ReadInt(kFaceYunetTopK),
			    .sface_metric          = metric,
			};
			config.sface_threshold = source.ReadSfaceThreshold(kFaceSfaceThreshold, metric);
			return config;
		}

		auto ReadDebugConfig(const RuntimeValueSource &source) -> DebugConfig {
			return {.end_report = source.ReadBool(config_schema::OptionId::kDebugEndReport)};
		}

		auto PopulateRuntimeConfig(const ConfigReader &reader) -> std::optional<RuntimeConfig> {
			const RuntimeValueSource source{.reader = &reader};
			const auto               metric = source.ReadFaceMetric();
			if (!metric.has_value()) {
				return std::nullopt;
			}

			RuntimeConfig config;
			config.core  = ReadCoreConfig(source);
			config.video = ReadVideoConfig(source);
			config.face  = ReadFaceConfig(source, *metric);
			config.debug = ReadDebugConfig(source);
			return config;
		}

	}  // namespace

	RuntimeConfig::RuntimeConfig()
	    : core(ReadCoreConfig(RuntimeValueSource{}))
	    , video(ReadVideoConfig(RuntimeValueSource{}))
	    , face(ReadFaceConfig(RuntimeValueSource{}, config_schema::kSfaceDefaultMetric))
	    , debug(ReadDebugConfig(RuntimeValueSource{})) {}

	auto DefaultVideoConfig() -> VideoConfig {
		return ReadVideoConfig(RuntimeValueSource{});
	}

	auto DefaultFaceConfig() -> FaceConfig {
		return ReadFaceConfig(RuntimeValueSource{}, config_schema::kSfaceDefaultMetric);
	}

	auto LoadRuntimeConfig(const std::filesystem::path &config_path,
	                       const std::optional<uid_t>   owner_uid) -> RuntimeConfigLoadResult {
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

		const auto security = CheckSecureConfigFd(fd.Get(), config_path, owner_uid);
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
