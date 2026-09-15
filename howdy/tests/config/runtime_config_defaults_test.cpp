#include "config/runtime_config.hpp"
#include "test_support.hpp"

using howdy::test::Expect;

auto main() -> int {
	const howdy::native::RuntimeConfig config;
	const auto                         video = howdy::native::DefaultVideoConfig();
	const auto                         face  = howdy::native::DefaultFaceConfig();
	bool                               ok    = true;
	ok &= Expect(config.video.timeout == video.timeout &&
	                 config.video.device_path == video.device_path,
	             "standalone runtime values retain video defaults");
	ok &= Expect(config.face.sface_metric == face.sface_metric &&
	                 config.face.sface_threshold == face.sface_threshold,
	             "standalone runtime values retain face defaults");
	ok &= Expect(howdy::native::FaceConfig{}.sface_metric == face.sface_metric,
	             "aggregate and runtime metric defaults agree");
	return ok ? 0 : 1;
}
