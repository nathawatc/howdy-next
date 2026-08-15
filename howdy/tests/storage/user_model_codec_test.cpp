#include "storage/user_model_codec_test_support.hpp"

#include <exception>
#include <iostream>

using namespace howdy::test::user_model_codec;

auto main() -> int {
	try {
		bool ok = true;
		ok &= expect_valid_strict_document();
		ok &= expect_new_entry_round_trip();
		ok &= expect_strict_parser_behavior();
		ok &= expect_duplicate_key_behavior();
		ok &= expect_immutable_to_mutable_conversion();
		ok &= expect_malformed_scalar_fields();
		ok &= expect_strict_shape_contract();
		ok &= expect_compatibility_checks();
		ok &= expect_encoding_validation();
		ok &= expect_additional_shape_edges();
		ok &= expect_document_edge_operations();
		ok &= expect_boundaries_and_limits();
		ok &= expect_multi_model_behavior();
		ok &= expect_unknown_field_preservation();
		ok &= expect_json_depth_limit();
		return ok ? 0 : 1;
	} catch (const std::exception &error) {
		std::cerr << "FAIL: unexpected exception: " << error.what() << "\n";
		return 1;
	} catch (...) {
		std::cerr << "FAIL: unexpected non-standard exception\n";
		return 1;
	}
}
