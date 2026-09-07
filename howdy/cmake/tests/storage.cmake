howdy_add_native_test(
	howdy_model_file_test
	native-model-file
	tests/model_assets/model_file_test.cpp
)
target_link_libraries(howdy_model_file_test PRIVATE howdy_model_file)

howdy_add_native_test(
	howdy_model_provenance_test
	native-model-provenance
	tests/model_assets/opencv_model_provenance_test.cpp
)
target_compile_definitions(
	howdy_model_provenance_test
	PRIVATE "HOWDY_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\""
)

howdy_add_native_test(
	howdy_user_models_test
	native-user-models
	tests/storage/user_models_test.cpp
	tests/storage/user_models_mutation_test.cpp
	tests/storage/user_models_failure_test.cpp
)
target_link_libraries(
	howdy_user_models_test
	PRIVATE
		howdy_user_models
		howdy_user_model_readiness
)

howdy_add_native_test(
	howdy_user_model_codec_test
	native-user-model-codec
	tests/storage/user_model_codec_test.cpp
	tests/storage/user_model_codec_parsing_test.cpp
	tests/storage/user_model_codec_document_test.cpp
	tests/storage/user_model_codec_limits_test.cpp
)
target_link_libraries(
	howdy_user_model_codec_test PRIVATE howdy_user_model_codec
)
