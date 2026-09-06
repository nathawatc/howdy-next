add_executable(howdy_model_file_test tests/model_assets/model_file_test.cpp)
howdy_configure_native_test_target(howdy_model_file_test)
target_link_libraries(howdy_model_file_test PRIVATE howdy_model_file)
add_test(NAME native-model-file COMMAND howdy_model_file_test)

add_executable(
	howdy_model_provenance_test
	tests/model_assets/opencv_model_provenance_test.cpp
)
howdy_configure_native_test_target(howdy_model_provenance_test)
target_compile_definitions(
	howdy_model_provenance_test
	PRIVATE "HOWDY_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\""
)
add_test(NAME native-model-provenance COMMAND howdy_model_provenance_test)

add_executable(
	howdy_user_models_test
	tests/storage/user_models_test.cpp
	tests/storage/user_models_mutation_test.cpp
	tests/storage/user_models_failure_test.cpp
)
howdy_configure_native_test_target(howdy_user_models_test)
target_link_libraries(
	howdy_user_models_test
	PRIVATE
		howdy_user_models
		howdy_user_model_readiness
)
add_test(NAME native-user-models COMMAND howdy_user_models_test)

add_executable(
	howdy_user_model_codec_test
	tests/storage/user_model_codec_test.cpp
	tests/storage/user_model_codec_parsing_test.cpp
	tests/storage/user_model_codec_document_test.cpp
	tests/storage/user_model_codec_limits_test.cpp
)
howdy_configure_native_test_target(howdy_user_model_codec_test)
target_link_libraries(
	howdy_user_model_codec_test PRIVATE howdy_user_model_codec
)
add_test(NAME native-user-model-codec COMMAND howdy_user_model_codec_test)
