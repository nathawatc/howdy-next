#include "config/runtime_config.hpp"

#include "config/config_reader.hpp"
#include "config/config_schema.hpp"
#include "config/config_utils.hpp"
#include "config/config_validation.hpp"
#include "config/config_values.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace howdy::native {
	namespace {

		template <RuntimeConfigLoadStatus status>
		auto failure_result(const std::filesystem::path &path, std::string error_message,
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

		auto success_result(const std::filesystem::path &path, RuntimeConfig config)
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

			[[nodiscard]] auto read_bool(config_schema::OptionId id) const -> bool {
				return reader == nullptr ? default_bool(id) : read_runtime_bool(*reader, id);
			}

			[[nodiscard]] auto read_int(config_schema::OptionId id) const -> int {
				return reader == nullptr ? default_int(id) : read_runtime_int(*reader, id);
			}

			[[nodiscard]] auto read_float(config_schema::OptionId id) const -> float {
				return reader == nullptr ? default_float(id) : read_runtime_float(*reader, id);
			}

			[[nodiscard]] auto read_string(config_schema::OptionId id) const -> std::string {
				return reader == nullptr ? default_string(id) : read_runtime_string(*reader, id);
			}

			[[nodiscard]] auto read_sface_threshold(config_schema::OptionId id,
			                                        std::string_view        metric) const -> float {
				return reader == nullptr ? default_float(id)
				                         : howdy::native::read_sface_threshold(*reader, metric);
			}

		private:
			static auto default_bool(config_schema::OptionId id) -> bool {
				return config_schema::runtime_default_bool(id);
			}

			static auto default_int(config_schema::OptionId id) -> int {
				return config_schema::runtime_default_int(id);
			}

			static auto default_float(config_schema::OptionId id) -> float {
				return config_schema::runtime_default_float(id);
			}

			static auto default_string(config_schema::OptionId id) -> std::string {
				return std::string(config_schema::runtime_default_string(id));
			}
		};

		auto read_core_config(const RuntimeValueSource &source) -> CoreConfig {
			using enum config_schema::OptionId;
			return {
			    .detection_notice    = source.read_bool(core_detection_notice),
			    .no_confirmation     = source.read_bool(core_no_confirmation),
			    .abort_if_ssh        = source.read_bool(core_abort_if_ssh),
			    .abort_if_lid_closed = source.read_bool(core_abort_if_lid_closed),
			    .disabled            = source.read_bool(core_disabled),
			};
		}

		auto read_video_config(const RuntimeValueSource &source) -> VideoConfig {
			using enum config_schema::OptionId;
			return {
			    .timeout              = source.read_int(video_timeout),
			    .device_path          = source.read_string(video_device_path),
			    .warn_no_device       = source.read_bool(video_warn_no_device),
			    .max_height           = source.read_float(video_max_height),
			    .frame_width          = source.read_int(video_frame_width),
			    .frame_height         = source.read_int(video_frame_height),
			    .clahe_enabled        = source.read_bool(video_clahe_enabled),
			    .clahe_clip_limit     = source.read_float(video_clahe_clip_limit),
			    .clahe_tile_grid_size = source.read_int(video_clahe_tile_grid_size),
			    .dark_threshold       = source.read_float(video_dark_threshold),
			    .force_mjpeg          = source.read_bool(video_force_mjpeg),
			    .exposure             = source.read_int(video_exposure),
			    .device_fps           = source.read_int(video_device_fps),
			    .rotate               = source.read_int(video_rotate),
			};
		}

		auto read_face_config(const RuntimeValueSource &source) -> FaceConfig {
			using enum config_schema::OptionId;
			FaceConfig config{
			    .yunet_score_threshold = source.read_float(face_yunet_score_threshold),
			    .yunet_nms_threshold   = source.read_float(face_yunet_nms_threshold),
			    .yunet_top_k           = source.read_int(face_yunet_top_k),
			    .sface_metric          = source.read_string(face_sface_metric),
			};
			config.sface_threshold =
			    source.read_sface_threshold(face_sface_threshold, config.sface_metric);
			return config;
		}

		auto read_debug_config(const RuntimeValueSource &source) -> DebugConfig {
			return {.end_report = source.read_bool(config_schema::OptionId::debug_end_report)};
		}

		auto populate_runtime_config(const ConfigReader &reader) -> RuntimeConfig {
			RuntimeConfig            config;
			const RuntimeValueSource source{.reader = &reader};
			config.core  = read_core_config(source);
			config.video = read_video_config(source);
			config.face  = read_face_config(source);
			config.debug = read_debug_config(source);
			return config;
		}

	}  // namespace

	RuntimeConfig::RuntimeConfig()
	    : core(read_core_config(RuntimeValueSource{}))
	    , video(read_video_config(RuntimeValueSource{}))
	    , face(read_face_config(RuntimeValueSource{}))
	    , debug(read_debug_config(RuntimeValueSource{})) {}

	auto default_video_config() -> VideoConfig {
		return read_video_config(RuntimeValueSource{});
	}

	auto default_face_config() -> FaceConfig {
		return read_face_config(RuntimeValueSource{});
	}

	auto load_runtime_config(const std::filesystem::path &config_path,
	                         const std::optional<uid_t>   owner_uid) -> RuntimeConfigLoadResult {
		const auto security = check_secure_config_path(config_path, owner_uid);
		if (!security.ok) {
			return failure_result<RuntimeConfigLoadStatus::kPathError>(
			    config_path, security.error_message, security.error_code);
		}

		const ConfigReader reader(config_path.string());
		if (!reader.ok()) {
			return failure_result<RuntimeConfigLoadStatus::kParseError>(
			    config_path, "Failed to parse config: " + config_path.string() + " (error " +
			                     std::to_string(reader.parse_error()) + ")");
		}

		if (const auto validation = validate_runtime_config(reader)) {
			return failure_result<RuntimeConfigLoadStatus::kInvalidRuntimeValue>(
			    config_path,
			    "Invalid runtime config in " + config_path.string() + ": " + *validation);
		}

		return success_result(config_path, populate_runtime_config(reader));
	}
}  // namespace howdy::native
