#include "../../src/config/config_document/internal.hpp"
#include "../../src/config/config_validation/internal.hpp"
#include "../../src/config/runtime_config_loader/internal.hpp"
#include "config/config_document_test_support.hpp"
#include "config/config_schema.hpp"
#include "test_support.hpp"

#include <array>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

#ifndef HOWDY_PACKAGED_CONFIG_PATH
#	error "HOWDY_PACKAGED_CONFIG_PATH must be defined by CMake"
#endif

namespace {

	using howdy::test::Expect;
	using howdy::test::WriteFile;

	auto SchemaOptionIdsAreUniqueAndResolvable() -> bool {
		using howdy::native::config_schema::OptionId;

		bool           ok           = true;
		const auto     options      = howdy::native::config_schema::RuntimeConfigOptions();
		constexpr auto option_count = static_cast<std::size_t>(OptionId::kCount);
		auto           seen         = std::array<bool, option_count>{};

		const auto validation = howdy::native::config_schema::ValidateOptions(options);
		ok &= Expect(!validation.has_value(), "production schema passes canonical validation");
		ok &= Expect(options.size() == option_count, "schema option count matches OptionId::count");
		if (validation.has_value()) {
			return ok;
		}
		for (const auto &option : options) {
			const auto index = static_cast<std::size_t>(std::to_underlying(option.id));
			const auto name  = OptionName(option);
			seen.at(index)   = true;

			const auto &resolved = howdy::native::config_schema::RuntimeConfigOption(option.id);
			ok &= Expect(&resolved == &option, "schema option id resolves same option: " + name);
			ok &= Expect(!option.description.empty(), "schema option has description: " + name);
			ok &= Expect(option.description != option.key,
			             "schema option description explains key: " + name);
		}

		for (std::size_t index = 0; index < option_count; ++index) {
			ok &= Expect(seen.at(index), "schema option id is covered: " + std::to_string(index));
		}
		return ok;
	}

	auto FallbackValue(const howdy::native::config_schema::Option &option) -> std::string {
		const auto fallback = howdy::native::config_schema::FallbackConfigValue(option);
		return fallback.has_value() ? howdy::native::config_schema::FormatConfigValue(*fallback)
		                            : std::string();
	}

	auto PackagedConfigMatchesSchema(const howdy::native::ConfigDocument &config) -> bool {
		bool        ok                  = true;
		std::size_t parsed_option_count = 0;
		for (const auto &option : howdy::native::config_schema::RuntimeConfigOptions()) {
			const auto section = std::string(option.section);
			const auto key     = std::string(option.key);
			const auto name    = OptionName(option);
			if (config.Value(section, key) == nullptr) {
				ok &= Expect(false, "packaged config contains schema option: " + name);
				continue;
			}
			++parsed_option_count;

			const auto  fallback = howdy::native::config_schema::FallbackConfigValue(option);
			const auto *shipped  = config.Value(section, key);
			const bool  matches =
			    fallback.has_value() && shipped != nullptr && *shipped == *fallback;
			auto message = name;
			message += " fallback=";
			message += FallbackValue(option);
			message += " shipped=";
			message += shipped == nullptr
			               ? std::string("<unset>")
						   : howdy::native::config_schema::FormatConfigValue(*shipped);
			ok &= Expect(matches, message);
		}

		ok &= Expect(parsed_option_count ==
		                 howdy::native::config_schema::RuntimeConfigOptions().size(),
		             "packaged config contains each schema option exactly once");
		return ok;
	}

	auto Validates(const std::filesystem::path &path, const std::string &content) -> bool {
		if (!WriteFile(path, content)) {
			return false;
		}
		const auto config = howdy::native::test_support::ReadConfigDocument(path);
		return config.Ok() && !howdy::native::ValidateRuntimeConfig(config).has_value();
	}

	auto Rejects(const std::filesystem::path &path, const std::string &content, const char *needle)
	    -> bool {
		if (!WriteFile(path, content)) {
			return false;
		}
		const auto config = howdy::native::test_support::ReadConfigDocument(path);
		if (!config.Ok()) {
			// Wrong TOML types fail structurally before semantic validation.
			return config.Error().contains(needle);
		}
		const auto validation = howdy::native::ValidateRuntimeConfig(config);
		return validation.has_value() && validation->contains(needle);
	}

	auto RejectsExact(const std::filesystem::path &path, const std::string &content,
	                  std::string_view expected) -> bool {
		if (!WriteFile(path, content)) {
			return false;
		}
		const auto config = howdy::native::test_support::ReadConfigDocument(path);
		if (!config.Ok()) {
			return false;
		}
		const auto validation = howdy::native::ValidateRuntimeConfig(config);
		return validation.has_value() && *validation == expected;
	}

}  // namespace

namespace {

	auto RunTests() -> int {
		namespace fs = std::filesystem;

		bool            ok              = true;
		const auto      packaged_config = fs::path(HOWDY_PACKAGED_CONFIG_PATH);
		const auto      temp_root       = fs::current_path() / "howdy-config-validation-test";
		std::error_code ec;
		fs::remove_all(temp_root, ec);
		fs::create_directories(temp_root, ec);
		ok &= Expect(!ec, "create temp root");

		ok &= Expect(SchemaOptionIdsAreUniqueAndResolvable(),
		             "schema option ids are unique and resolvable");
		const auto packaged_reader =
		    howdy::native::test_support::ReadConfigDocument(packaged_config);
		ok &= Expect(packaged_reader.Ok(), "packaged config parses");
		if (packaged_reader.Ok()) {
			ok &= Expect(!howdy::native::ValidateRuntimeConfig(packaged_reader).has_value(),
			             "packaged config values validate against schema");
			ok &= Expect(PackagedConfigMatchesSchema(packaged_reader),
			             "packaged config matches schema fallbacks");
		}

		ok &= Expect(Validates(temp_root / "device-fps-zero.toml", "[video]\ndevice_fps = 0\n"),
		             "device_fps zero is valid");
		ok &= Expect(Rejects(temp_root / "device-fps-negative.toml", "[video]\ndevice_fps = -1\n",
		                     "device_fps"),
		             "device_fps negative is invalid");
		ok &= Expect(Rejects(temp_root / "device-fps-text.toml", "[video]\ndevice_fps = \"fast\"\n",
		                     "device_fps"),
		             "invalid integer values are rejected");
		ok &= Expect(Rejects(temp_root / "float-text.toml",
		                     "[video]\nclahe_clip_limit = \"fast\"\n", "clahe_clip_limit"),
		             "invalid float values are rejected");
		ok &= Expect(
		    Rejects(temp_root / "bool-text.toml", "[core]\ndisabled = \"maybe\"\n", "disabled"),
		    "invalid boolean values are rejected");

		struct BoundaryCase {
			const char *name;
			const char *section;
			const char *key;
			const char *minimum;
			const char *maximum;
			const char *below;
			const char *above;
		};

		const std::array<BoundaryCase, 10> boundary_cases = {
		    {
		        {
		            .name    = "timeout",
		            .section = "video",
		            .key     = "timeout",
		            .minimum = "1",
		            .maximum = "300",
		            .below   = "0",
		            .above   = "301",
		        },
		        {
		            .name    = "max-height",
		            .section = "video",
		            .key     = "max_height",
		            .minimum = "32",
		            .maximum = "4096",
		            .below   = "31.99",
		            .above   = "4096.01",
		        },
		        {
		            .name    = "rotate",
		            .section = "video",
		            .key     = "rotate",
		            .minimum = "0",
		            .maximum = "3",
		            .below   = "-1",
		            .above   = "4",
		        },
		        {
		            .name    = "dark-threshold",
		            .section = "video",
		            .key     = "dark_threshold",
		            .minimum = "0",
		            .maximum = "99.9",
		            .below   = "-0.1",
		            .above   = "100",
		        },
		        {
		            .name    = "clahe-clip-limit",
		            .section = "video",
		            .key     = "clahe_clip_limit",
		            .minimum = "0.01",
		            .maximum = "100",
		            .below   = "0",
		            .above   = "100.01",
		        },
		        {
		            .name    = "clahe-tile-grid-size",
		            .section = "video",
		            .key     = "clahe_tile_grid_size",
		            .minimum = "1",
		            .maximum = "64",
		            .below   = "0",
		            .above   = "65",
		        },
		        {
		            .name    = "yunet-score-threshold",
		            .section = "face",
		            .key     = "yunet_score_threshold",
		            .minimum = "0",
		            .maximum = "1",
		            .below   = "-0.1",
		            .above   = "1.1",
		        },
		        {
		            .name    = "yunet-nms-threshold",
		            .section = "face",
		            .key     = "yunet_nms_threshold",
		            .minimum = "0",
		            .maximum = "1",
		            .below   = "-0.1",
		            .above   = "1.1",
		        },
		        {
		            .name    = "yunet-top-k",
		            .section = "face",
		            .key     = "yunet_top_k",
		            .minimum = "1",
		            .maximum = "10000",
		            .below   = "0",
		            .above   = "10001",
		        },
		        {
		            .name    = "device-fps",
		            .section = "video",
		            .key     = "device_fps",
		            .minimum = "0",
		            .maximum = "480",
		            .below   = "-1",
		            .above   = "481",
		        },
		    },
		};

		for (const auto &test : boundary_cases) {
			const auto prefix = std::string("[") + test.section + "]\n" + test.key + " = ";
			ok &= Expect(Validates(temp_root / (std::string(test.name) + "-minimum.toml"),
			                       prefix + test.minimum + "\n"),
			             std::string(test.name) + " minimum boundary is valid");
			ok &= Expect(Validates(temp_root / (std::string(test.name) + "-maximum.toml"),
			                       prefix + test.maximum + "\n"),
			             std::string(test.name) + " maximum boundary is valid");
			ok &= Expect(Rejects(temp_root / (std::string(test.name) + "-below.toml"),
			                     prefix + test.below + "\n", test.key),
			             std::string(test.name) + " below minimum is invalid");
			ok &= Expect(Rejects(temp_root / (std::string(test.name) + "-above.toml"),
			                     prefix + test.above + "\n", test.key),
			             std::string(test.name) + " above maximum is invalid");
		}

		for (const auto *const key : {"frame_width", "frame_height"}) {
			const auto prefix = std::string("[video]\n") + key + " = ";
			ok &= Expect(
			    Validates(temp_root / (std::string(key) + "-sentinel.toml"), prefix + "-1\n"),
			    std::string(key) + " -1 sentinel is valid");
			ok &=
			    Expect(Validates(temp_root / (std::string(key) + "-minimum.toml"), prefix + "16\n"),
				       std::string(key) + " minimum boundary is valid");
			ok &= Expect(
			    Validates(temp_root / (std::string(key) + "-maximum.toml"), prefix + "8192\n"),
			    std::string(key) + " maximum boundary is valid");
			ok &= Expect(
			    Rejects(temp_root / (std::string(key) + "-below.toml"), prefix + "15\n", key),
			    std::string(key) + " below minimum is invalid");
			ok &= Expect(
			    Rejects(temp_root / (std::string(key) + "-above.toml"), prefix + "8193\n", key),
			    std::string(key) + " above maximum is invalid");
		}

		ok &= Expect(Validates(temp_root / "exposure-sentinel.toml", "[video]\nexposure = -1\n"),
		             "exposure -1 sentinel is valid");
		ok &= Expect(Validates(temp_root / "exposure-minimum.toml", "[video]\nexposure = 0\n"),
		             "exposure minimum boundary is valid");
		ok &= Expect(Validates(temp_root / "exposure-maximum.toml", "[video]\nexposure = 10000\n"),
		             "exposure maximum boundary is valid");
		ok &= Expect(
		    Rejects(temp_root / "exposure-below.toml", "[video]\nexposure = -2\n", "exposure"),
		    "exposure below sentinel is invalid");
		ok &= Expect(
		    Rejects(temp_root / "exposure-above.toml", "[video]\nexposure = 10001\n", "exposure"),
		    "exposure above maximum is invalid");

		for (const auto *const value : {"nan", "+inf", "-inf"}) {
			ok &= Expect(Rejects(temp_root / (std::string("non-finite-") + value + ".toml"),
			                     "[face]\nyunet_score_threshold = " + std::string(value) + "\n",
			                     "yunet_score_threshold"),
			             std::string("non-finite float is rejected: ") + value);
		}

		ok &= Expect(Validates(temp_root / "cosine-threshold-min.toml",
		                       "[face]\nsface_metric = \"cosine\"\nsface_threshold = 0\n"),
		             "cosine threshold minimum boundary is valid");
		ok &= Expect(Validates(temp_root / "cosine-threshold-max.toml",
		                       "[face]\nsface_metric = \"cosine\"\nsface_threshold = 1\n"),
		             "cosine threshold maximum boundary is valid");
		ok &= Expect(
		    RejectsExact(temp_root / "cosine-threshold-over.toml",
			             "[face]\nsface_metric = \"cosine\"\nsface_threshold = 1.001\n",
			             "Invalid config value for sface_threshold=\"1.001\": expected range 0..1"),
		    "cosine threshold above maximum keeps validation message");
		ok &= Expect(Validates(temp_root / "l2-threshold-max.toml",
		                       "[face]\nsface_metric = \"l2\"\nsface_threshold = 4\n"),
		             "l2 threshold maximum boundary is valid");
		ok &= Expect(
		    RejectsExact(temp_root / "l2-threshold-over.toml",
			             "[face]\nsface_metric = \"l2\"\nsface_threshold = 4.001\n",
			             "Invalid config value for sface_threshold=\"4.001\": expected range 0..4"),
		    "l2 threshold above maximum keeps validation message");
		ok &= Expect(Validates(temp_root / "l2norm-threshold-max.toml",
		                       "[face]\nsface_metric = \"l2norm\"\nsface_threshold = 2\n"),
		             "normalized l2norm threshold maximum is valid");
		ok &= Expect(
		    RejectsExact(temp_root / "l2norm-threshold-over.toml",
			             "[face]\nsface_metric = \"l2norm\"\nsface_threshold = 2.001\n",
			             "Invalid config value for sface_threshold=\"2.001\": expected range 0..2"),
		    "l2norm threshold above unit-vector distance bound is rejected");

		for (const auto *const metric : {"cosine", "l2", "l2norm"}) {
			ok &= Expect(Validates(temp_root / (std::string("metric-") + metric + ".toml"),
			                       "[face]\nsface_metric = \"" + std::string(metric) + "\"\n"),
			             std::string("sface metric is accepted: ") + metric);
		}
		for (const auto *const metric : {"COSINE", "L2", "L2NORM"}) {
			ok &= Expect(!Validates(temp_root / (std::string("metric-upper-") + metric + ".toml"),
			                        "[face]\nsface_metric = \"" + std::string(metric) + "\"\n"),
			             std::string("sface metric is case-sensitive: ") + metric);
		}
		ok &=
		    Expect(RejectsExact(temp_root / "invalid-metric.toml",
			                    "[face]\nsface_metric = \"euclidean\"\n",
			                    "Invalid config value for sface_metric=\"euclidean\": expected one "
			                    "of: cosine, l2, l2norm"),
			       "unknown sface metric is rejected through existing validation contract");

		for (const auto *const device_path : {
		         "none",
		         "/dev/video0",
		         "/dev/v4l/by-path/platform-camera",
		         "/dev/v4l/by-id/usb-camera",
		     }) {
			ok &= Expect(
			    Validates(temp_root / (std::string("device-path-") +
				                       std::to_string(std::string(device_path).size()) + ".toml"),
				          "[video]\ndevice_path = \"" + std::string(device_path) + "\"\n"),
			    std::string("device path is accepted: ") + device_path);
		}
		ok &= Expect(Rejects(temp_root / "unrelated-device-path.toml",
		                     "[video]\ndevice_path = \"/tmp/camera\"\n", "device_path"),
		             "unrelated device path is rejected");
		ok &=
		    Expect(Rejects(temp_root / "traversal-by-id-device-path.toml",
			               "[video]\ndevice_path = \"/dev/v4l/by-id/../../null\"\n", "device_path"),
			       "traversal by-id device path is rejected");

		const std::array<std::pair<const char *, const char *>, 4> known_runtime_options = {
		    {
		        {"core", "detection_notice"},
		        {"video", "timeout"},
		        {"face", "yunet_score_threshold"},
		        {"debug", "end_report"},
		    },
		};
		for (const auto &[section, key] : known_runtime_options) {
			ok &= Expect(howdy::native::config_schema::RuntimeConfigOption(section, key) != nullptr,
			             std::string("runtime config option present: ") + section + "." + key);
		}
		ok &= Expect(Rejects(temp_root / "unknown-key.toml", "[core]\nunknown_core_key = true\n",
		                     "unknown key \"core.unknown_core_key\""),
		             "unknown config keys are rejected");
		ok &= Expect(Rejects(temp_root / "unknown-table.toml", "[camera]\ntimeout = 4\n",
		                     "unknown table [camera]"),
		             "unknown config tables are rejected");

		const auto invalid_runtime = temp_root / "invalid-runtime.toml";
		ok &= Expect(WriteFile(invalid_runtime, "[video]\ntimeout = 0\n"), "write invalid runtime");
		const auto invalid_config =
		    howdy::native::test_support::ReadConfigDocument(invalid_runtime);
		ok &= Expect(invalid_config.Ok(), "out-of-range runtime config is structurally valid");
		ok &= Expect(howdy::native::ValidateRuntimeConfig(invalid_config).has_value(),
		             "invalid runtime config fails closed before runtime use");
		ok &=
		    Expect(howdy::native::ReadRuntimeInt(
		               invalid_config, howdy::native::config_schema::OptionId::kVideoTimeout) == 4,
			       "unsafe invalid timeout falls back to current default value");

		fs::remove_all(temp_root, ec);

		return ok ? 0 : 1;
	}

}  // namespace

auto main() -> int {
	try {
		return RunTests();
	} catch (const std::exception &error) {
		std::cerr << "FAIL: unexpected exception: " << error.what() << '\n';
	} catch (...) {
		std::cerr << "FAIL: unexpected non-standard exception\n";
	}
	return 1;
}
