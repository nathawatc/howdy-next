#include "config/config_reader.hpp"
#include "config/config_validation.hpp"
#include "config/config_values.hpp"
#include "config/number_parsing.hpp"
#include "test_support.hpp"

#include <array>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

#ifndef HOWDY_PACKAGED_CONFIG_PATH
#	define HOWDY_PACKAGED_CONFIG_PATH "config/config.ini"
#endif

namespace {

	using howdy::test::expect;

	auto write_file(const std::filesystem::path &path, const std::string &content) -> bool {
		std::ofstream out(path);
		if (!out.is_open()) {
			return false;
		}
		out << content;
		return out.good();
	}

	auto trim(std::string_view value) -> std::string_view {
		while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
			value.remove_prefix(1);
		}
		while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
			value.remove_suffix(1);
		}
		return value;
	}

	auto option_name(const howdy::native::config_schema::Option &option) -> std::string {
		return std::string(option.section) + "." + std::string(option.key);
	}

	auto schema_option_ids_are_unique_and_resolvable() -> bool {
		using howdy::native::config_schema::OptionId;

		bool           ok           = true;
		const auto     options      = howdy::native::config_schema::runtime_config_options();
		constexpr auto option_count = static_cast<std::size_t>(OptionId::count);
		auto           seen         = std::array<bool, option_count>{};

		ok &= expect(options.size() == option_count, "schema option count matches OptionId::count");
		for (const auto &option : options) {
			const auto index = static_cast<std::size_t>(std::to_underlying(option.id));
			const auto name  = option_name(option);
			ok &= expect(index < option_count, "schema option id is in range: " + name);
			if (index >= option_count) {
				continue;
			}
			ok &= expect(!seen[index], "schema option id is unique: " + name);
			seen[index] = true;

			const auto &resolved = howdy::native::config_schema::runtime_config_option(option.id);
			ok &= expect(&resolved == &option, "schema option id resolves same option: " + name);
		}

		for (std::size_t index = 0; index < option_count; ++index) {
			ok &= expect(seen[index], "schema option id is covered: " + std::to_string(index));
		}
		return ok;
	}

	auto packaged_config_keys_are_known(const std::filesystem::path &path) -> bool {
		std::ifstream input(path);
		if (!input.is_open()) {
			std::cerr << "FAIL: open packaged config: " << path << "\n";
			return false;
		}

		bool        ok = true;
		std::string section;
		std::string line;
		while (std::getline(input, line)) {
			const auto comment = line.find('#');
			if (comment != std::string::npos) {
				line.resize(comment);
			}
			const auto stripped = trim(line);
			if (stripped.empty()) {
				continue;
			}
			if (stripped.front() == '[' && stripped.back() == ']') {
				section = std::string(trim(stripped.substr(1, stripped.size() - 2)));
				continue;
			}

			const auto separator = stripped.find('=');
			if (separator == std::string_view::npos) {
				ok &= expect(false, "packaged config line is not section or key-value: " +
				                        std::string(stripped));
				continue;
			}
			const auto key = trim(stripped.substr(0, separator));
			ok &= expect(
			    howdy::native::config_schema::runtime_config_option(section, key) != nullptr,
			    "packaged config key is known by schema: " + section + "." + std::string(key));
		}
		return ok;
	}

	auto validates(const std::filesystem::path &path, const std::string &content) -> bool {
		if (!write_file(path, content)) {
			return false;
		}
		const howdy::native::ConfigReader config(path.string());
		return config.ok() && !howdy::native::validate_runtime_config(config).has_value();
	}

	auto rejects(const std::filesystem::path &path, const std::string &content, const char *needle)
	    -> bool {
		if (!write_file(path, content)) {
			return false;
		}
		const howdy::native::ConfigReader config(path.string());
		if (!config.ok()) {
			return false;
		}
		const auto validation = howdy::native::validate_runtime_config(config);
		return validation.has_value() && validation->contains(needle);
	}

}  // namespace

auto main() -> int {
	namespace fs = std::filesystem;

	bool            ok              = true;
	const auto      packaged_config = fs::path(HOWDY_PACKAGED_CONFIG_PATH);
	const auto      temp_root       = fs::current_path() / "howdy-config-validation-test";
	std::error_code ec;
	fs::remove_all(temp_root, ec);
	fs::create_directories(temp_root, ec);
	ok &= expect(!ec, "create temp root");

	ok &= expect(schema_option_ids_are_unique_and_resolvable(),
	             "schema option ids are unique and resolvable");
	ok &= expect(packaged_config_keys_are_known(packaged_config),
	             "all packaged config keys are known by schema");
	const howdy::native::ConfigReader packaged_reader(packaged_config.string());
	ok &= expect(packaged_reader.ok(), "packaged config parses");
	if (packaged_reader.ok()) {
		ok &= expect(!howdy::native::validate_runtime_config(packaged_reader).has_value(),
		             "packaged config values validate against schema");
	}

	ok &= expect(validates(temp_root / "device-fps-zero.ini", "[video]\ndevice_fps = 0\n"),
	             "device_fps zero is valid");
	ok &= expect(
	    rejects(temp_root / "device-fps-negative.ini", "[video]\ndevice_fps = -1\n", "device_fps"),
	    "device_fps negative is invalid");
	ok &= expect(
	    rejects(temp_root / "device-fps-text.ini", "[video]\ndevice_fps = fast\n", "device_fps"),
	    "invalid integer values are rejected");
	ok &= expect(rejects(temp_root / "float-text.ini", "[video]\nclahe_clip_limit = fast\n",
	                     "clahe_clip_limit"),
	             "invalid float values are rejected");
	ok &= expect(rejects(temp_root / "bool-text.ini", "[core]\ndisabled = maybe\n", "disabled"),
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

	const std::array<BoundaryCase, 10> boundary_cases = {{
	    {.name    = "timeout",
	     .section = "video",
	     .key     = "timeout",
	     .minimum = "1",
	     .maximum = "300",
	     .below   = "0",
	     .above   = "301"},
	    {.name    = "max-height",
	     .section = "video",
	     .key     = "max_height",
	     .minimum = "32",
	     .maximum = "4096",
	     .below   = "31.99",
	     .above   = "4096.01"},
	    {.name    = "rotate",
	     .section = "video",
	     .key     = "rotate",
	     .minimum = "0",
	     .maximum = "2",
	     .below   = "-1",
	     .above   = "3"},
	    {.name    = "dark-threshold",
	     .section = "video",
	     .key     = "dark_threshold",
	     .minimum = "0",
	     .maximum = "99.9",
	     .below   = "-0.1",
	     .above   = "100"},
	    {.name    = "clahe-clip-limit",
	     .section = "video",
	     .key     = "clahe_clip_limit",
	     .minimum = "0.01",
	     .maximum = "100",
	     .below   = "0",
	     .above   = "100.01"},
	    {.name    = "clahe-tile-grid-size",
	     .section = "video",
	     .key     = "clahe_tile_grid_size",
	     .minimum = "1",
	     .maximum = "64",
	     .below   = "0",
	     .above   = "65"},
	    {.name    = "yunet-score-threshold",
	     .section = "face",
	     .key     = "yunet_score_threshold",
	     .minimum = "0",
	     .maximum = "1",
	     .below   = "-0.1",
	     .above   = "1.1"},
	    {.name    = "yunet-nms-threshold",
	     .section = "face",
	     .key     = "yunet_nms_threshold",
	     .minimum = "0",
	     .maximum = "1",
	     .below   = "-0.1",
	     .above   = "1.1"},
	    {.name    = "yunet-top-k",
	     .section = "face",
	     .key     = "yunet_top_k",
	     .minimum = "1",
	     .maximum = "10000",
	     .below   = "0",
	     .above   = "10001"},
	    {.name    = "device-fps",
	     .section = "video",
	     .key     = "device_fps",
	     .minimum = "0",
	     .maximum = "480",
	     .below   = "-1",
	     .above   = "481"},
	}};

	for (const auto &test : boundary_cases) {
		const auto prefix = std::string("[") + test.section + "]\n" + test.key + " = ";
		ok &= expect(validates(temp_root / (std::string(test.name) + "-minimum.ini"),
		                       prefix + test.minimum + "\n"),
		             std::string(test.name) + " minimum boundary is valid");
		ok &= expect(validates(temp_root / (std::string(test.name) + "-maximum.ini"),
		                       prefix + test.maximum + "\n"),
		             std::string(test.name) + " maximum boundary is valid");
		ok &= expect(rejects(temp_root / (std::string(test.name) + "-below.ini"),
		                     prefix + test.below + "\n", test.key),
		             std::string(test.name) + " below minimum is invalid");
		ok &= expect(rejects(temp_root / (std::string(test.name) + "-above.ini"),
		                     prefix + test.above + "\n", test.key),
		             std::string(test.name) + " above maximum is invalid");
	}

	for (const auto *const key : {"frame_width", "frame_height"}) {
		const auto prefix = std::string("[video]\n") + key + " = ";
		ok &= expect(validates(temp_root / (std::string(key) + "-sentinel.ini"), prefix + "-1\n"),
		             std::string(key) + " -1 sentinel is valid");
		ok &= expect(validates(temp_root / (std::string(key) + "-minimum.ini"), prefix + "16\n"),
		             std::string(key) + " minimum boundary is valid");
		ok &= expect(validates(temp_root / (std::string(key) + "-maximum.ini"), prefix + "8192\n"),
		             std::string(key) + " maximum boundary is valid");
		ok &= expect(rejects(temp_root / (std::string(key) + "-below.ini"), prefix + "15\n", key),
		             std::string(key) + " below minimum is invalid");
		ok &= expect(rejects(temp_root / (std::string(key) + "-above.ini"), prefix + "8193\n", key),
		             std::string(key) + " above maximum is invalid");
	}

	ok &= expect(validates(temp_root / "exposure-sentinel.ini", "[video]\nexposure = -1\n"),
	             "exposure -1 sentinel is valid");
	ok &= expect(validates(temp_root / "exposure-minimum.ini", "[video]\nexposure = 0\n"),
	             "exposure minimum boundary is valid");
	ok &= expect(validates(temp_root / "exposure-maximum.ini", "[video]\nexposure = 10000\n"),
	             "exposure maximum boundary is valid");
	ok &= expect(rejects(temp_root / "exposure-below.ini", "[video]\nexposure = -2\n", "exposure"),
	             "exposure below sentinel is invalid");
	ok &=
	    expect(rejects(temp_root / "exposure-above.ini", "[video]\nexposure = 10001\n", "exposure"),
	           "exposure above maximum is invalid");

	for (const auto *const value : {"nan", "+inf", "-inf"}) {
		ok &= expect(rejects(temp_root / (std::string("non-finite-") + value + ".ini"),
		                     "[face]\nyunet_score_threshold = " + std::string(value) + "\n",
		                     "yunet_score_threshold"),
		             std::string("non-finite float is rejected: ") + value);
		ok &= expect(!howdy::native::parse_config_float_strict(value).has_value(),
		             std::string("strict parser rejects non-finite value: ") + value);
	}

	ok &= expect(validates(temp_root / "cosine-threshold-min.ini",
	                       "[face]\nsface_metric = cosine\nsface_threshold = 0\n"),
	             "cosine threshold minimum boundary is valid");
	ok &= expect(validates(temp_root / "cosine-threshold-max.ini",
	                       "[face]\nsface_metric = cosine\nsface_threshold = 1\n"),
	             "cosine threshold maximum boundary is valid");
	ok &= expect(rejects(temp_root / "cosine-threshold-over.ini",
	                     "[face]\nsface_metric = cosine\nsface_threshold = 1.001\n",
	                     "sface_threshold"),
	             "cosine threshold above maximum is invalid");
	ok &= expect(validates(temp_root / "l2-threshold-max.ini",
	                       "[face]\nsface_metric = l2\nsface_threshold = 4\n"),
	             "l2 threshold maximum boundary is valid");
	ok &= expect(rejects(temp_root / "l2-threshold-over.ini",
	                     "[face]\nsface_metric = l2\nsface_threshold = 4.001\n", "sface_threshold"),
	             "l2 threshold above maximum is invalid");
	ok &= expect(validates(temp_root / "l2norm-threshold-max.ini",
	                       "[face]\nsface_metric = L2NORM\nsface_threshold = 4\n"),
	             "normalized l2norm threshold maximum is valid");
	ok &= expect(rejects(temp_root / "l2norm-threshold-over.ini",
	                     "[face]\nsface_metric = L2NORM\nsface_threshold = 4.001\n",
	                     "sface_threshold"),
	             "normalized l2norm threshold above maximum is invalid");

	for (const auto *const metric : {"cosine", "COSINE", "l2", "L2", "l2norm", "L2NORM"}) {
		ok &= expect(validates(temp_root / (std::string("metric-") + metric + ".ini"),
		                       "[face]\nsface_metric = " + std::string(metric) + "\n"),
		             std::string("sface metric is accepted case-insensitively: ") + metric);
	}
	ok &= expect(rejects(temp_root / "invalid-metric.ini", "[face]\nsface_metric = euclidean\n",
	                     "sface_metric"),
	             "unknown sface metric is rejected");

	for (const auto *const device_path :
	     {"none", "/dev/video0", "/dev/v4l/by-path/platform-camera"}) {
		ok &=
		    expect(validates(temp_root / (std::string("device-path-") +
		                                  std::to_string(std::string(device_path).size()) + ".ini"),
		                     "[video]\ndevice_path = " + std::string(device_path) + "\n"),
		           std::string("device path is accepted: ") + device_path);
	}
	ok &= expect(rejects(temp_root / "unrelated-device-path.ini",
	                     "[video]\ndevice_path = /tmp/camera\n", "device_path"),
	             "unrelated device path is rejected");

	const std::array<std::pair<const char *, const char *>, 5> known_runtime_options = {{
	    {"core", "detection_notice"},
	    {"video", "timeout"},
	    {"face", "yunet_score_threshold"},
	    {"snapshots", "save_failed"},
	    {"debug", "end_report"},
	}};
	for (const auto &[section, key] : known_runtime_options) {
		ok &= expect(howdy::native::config_schema::runtime_config_option(section, key) != nullptr,
		             std::string("runtime config option present: ") + section + "." + key);
	}
	ok &= expect(validates(temp_root / "obsolete-keys.ini",
	                       "[core]\nworkaround = input\ngtk_stdout = true\n"),
	             "obsolete workaround and gtk_stdout config keys remain ignored");
	ok &= expect(validates(temp_root / "unknown-keys.ini", "[core]\nunknown_core_key = invalid\n"
	                                                       "[video]\nunknown_video_key = invalid\n"
	                                                       "[face]\nunknown_face_key = invalid\n"),
	             "unknown config keys remain ignored");

	const auto invalid_runtime = temp_root / "invalid-runtime.ini";
	ok &= expect(write_file(invalid_runtime, "[video]\ntimeout = abc\n"), "write invalid runtime");
	const howdy::native::ConfigReader invalid_config(invalid_runtime.string());
	ok &= expect(invalid_config.ok(), "invalid runtime config is syntactically parseable");
	ok &= expect(howdy::native::validate_runtime_config(invalid_config).has_value(),
	             "invalid runtime config fails closed before runtime use");
	ok &= expect(howdy::native::config_timeout_seconds(invalid_config) == 4,
	             "unsafe invalid timeout falls back to current default value");

	fs::remove_all(temp_root, ec);

	return ok ? 0 : 1;
}
