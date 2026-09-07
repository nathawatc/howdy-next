#pragma once

#include "vision/face_model/internal.hpp"

#include <utility>

namespace howdy::native {
	class FaceModelBackendFactory {
	public:
		using Backend = FaceModel::Backend;

		static auto Create(const FaceConfig &config, Backend backend) -> FaceModel {
			return {config, std::move(backend)};
		}
	};

	class FaceModelTestAccess {
	public:
		using Backend = FaceModelBackendFactory::Backend;

		static auto Create(const FaceConfig &config, Backend backend) -> FaceModel {
			return FaceModelBackendFactory::Create(config, std::move(backend));
		}
	};

}  // namespace howdy::native
