#include "compare/entrypoint/internal.hpp"

#include "vision/face_model.hpp"

auto howdy::native::compare_entrypoint_internal::WarmModel(FaceModel &model) -> bool {
	return model.WarmUpRecognizer();
}
