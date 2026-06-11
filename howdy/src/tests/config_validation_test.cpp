#include "config/config_reader.hpp"
#include "config/config_validation.hpp"
#include "config/config_values.hpp"
#include "config/number_parsing.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>

namespace {

	auto write_file(const std::filesystem::path &path, const std::string &content) -> bool {
		std::ofstream out(path);
		if (!out.is_open()) {
			return false;
		}
		out << content;
		return out.good();
	}

	auto expect(bool condition, const std::string &message) -> bool {
		if (!condition) {
			std::cerr << "FAIL: " << message << "\n";
			return false;
		}
		return true;
	}

	auto validates(const std::filesystem::path &path, const std::string &content) -> bool {
		if (!write_file(path, content)) {
			return false;
		}
		const howdy::native::ConfigReader config(path.string());
		return config.ok() && !howdy::native::validate_runtime_config(config).has_value();
	}

	auto rejects(const std::filesystem::path &path, const std::string &content,
	             const std::string &needle) -> bool {
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

	bool            ok        = true;
	const auto      temp_root = fs::current_path() / "howdy-config-validation-test";
	std::error_code ec;
	fs::remove_all(temp_root, ec);
	fs::create_directories(temp_root, ec);
	ok &= expect(!ec, "create temp root");

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

	ok &= expect(validates(temp_root / "absolute-model-path.ini",
	                       "[face]\nyunet_model = /opt/howdy/yunet.onnx\n"
	                       "sface_model = /opt/howdy/sface.onnx\n"),
	             "absolute custom model paths are accepted by validation");
	ok &= expect(validates(temp_root / "model-sentinels.ini",
	                       "[face]\nyunet_model = default\nsface_model = none\n"),
	             "model sentinels are accepted");
	ok &= expect(
	    validates(temp_root / "empty-model-paths.ini", "[face]\nyunet_model = \nsface_model = \n"),
	    "empty model path values remain accepted");
	ok &= expect(rejects(temp_root / "relative-yunet-path.ini",
	                     "[face]\nyunet_model = models/yunet.onnx\n", "yunet_model"),
	             "relative YuNet model path is rejected");
	ok &= expect(rejects(temp_root / "relative-sface-path.ini",
	                     "[face]\nsface_model = models/sface.onnx\n", "sface_model"),
	             "relative SFace model path is rejected");

	const std::array<std::pair<const char *, const char *>, 5> known_runtime_options = {{
	    {"core", "detection_notice"},
	    {"video", "timeout"},
	    {"face", "yunet_model"},
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
