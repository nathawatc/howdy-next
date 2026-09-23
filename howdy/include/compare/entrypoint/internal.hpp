#pragma once

namespace howdy::native {

	class FaceModel;

	namespace compare_entrypoint_internal {

		[[nodiscard]] auto WarmModel(FaceModel &model) -> bool;

	}  // namespace compare_entrypoint_internal

}  // namespace howdy::native
