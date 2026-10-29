#pragma once

#include "vision/face_model/internal.hpp"

#include <utility>

namespace howdy::native {
	class FaceModelTestAccess {
	public:
		using Backend = FaceModel::Backend;

		static auto Create(const FaceConfig &config, Backend backend) -> FaceModel {
			return {config, std::move(backend)};
		}
	};

}  // namespace howdy::native
