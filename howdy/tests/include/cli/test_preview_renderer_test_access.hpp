#pragma once

#include "cli/test/preview_renderer.hpp"
#include "config/runtime_config.hpp"
#include "storage/user_model_types.hpp"

#include <vector>

namespace howdy::native::test_cli_internal {
	class TestPreviewRendererTestAccess {
	public:
		static auto Config(const TestPreviewRenderer &renderer)
		    -> const howdy::native::VideoConfig & {
			return renderer.config_;
		}

		static auto Models(const TestPreviewRenderer &renderer)
		    -> const std::vector<howdy::native::EncodingModelInfo> & {
			return renderer.models_;
		}
	};

}  // namespace howdy::native::test_cli_internal
