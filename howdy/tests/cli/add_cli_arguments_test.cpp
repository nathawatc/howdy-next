#include "cli/add_cli_test_support.hpp"
#include "vision/face_model.hpp"

#include <iostream>
#include <sstream>

namespace howdy::test::add_cli {

	namespace {

		auto successful_enrollment_appends_expected_model() -> bool {
			auto      context = make_success_context();
			const int result  = run_add(context, {"howdy-add", "alice", "front-door"});

			bool ok = true;
			ok &= expect(result == 0, "successful add returns 0");
			ok &= expect(context.load_calls == 1, "runtime config loaded once");
			ok &= expect(context.preflight_calls == 1, "preflight callback called once");
			ok &= expect(context.preflight_user == "alice", "preflight callback receives user");
			ok &= expect(context.preflight_config.video.dark_threshold == 32.0F,
			             "preflight callback receives runtime config");
			ok &= expect(context.capture_calls == 1, "capture callback called once");
			ok &= expect(!context.capture_plain, "non-plain flag reaches capture callback");
			ok &= expect(context.capture_user == "alice", "capture callback receives user");
			ok &= expect(context.capture_config.video.dark_threshold == 32.0F,
			             "capture callback receives runtime config");
			ok &= expect(context.capture_label == "front-door", "capture callback receives label");
			ok &= expect(context.append_calls == 1, "append callback called once");
			ok &= expect(context.appended_user == "alice", "append callback receives user");
			ok &= expect(context.appended_entry.label == "front-door",
			             "append callback receives label");
			ok &= expect(context.appended_entry.backend == howdy::native::FaceModel::kBackendName,
			             "append callback receives backend");
			ok &= expect(context.appended_entry.metric == howdy::native::FaceMetric::kCosine,
			             "append callback receives metric");
			ok &= expect(context.appended_entry.model == howdy::native::FaceModel::kSfaceModel,
			             "append callback receives model");
			ok &= expect(context.appended_entry.encodings.size() == 1,
			             "append callback receives one encoding vector");
			ok &= expect(context.appended_entry.encodings.front().size() == 1,
			             "append callback receives one encoding value");
			ok &= expect(context.appended_entry.encodings.front().front() == 0.125F,
			             "append callback receives encoding");
			ok &= expect((context.events ==
			              std::vector<std::string>{"load", "preflight", "capture", "append"}),
			             "successful add orders callbacks");
			return ok;
		}

		auto successful_interactive_flow_prompts_after_preflight() -> bool {
			auto               context = make_success_context();
			std::istringstream input("front-door\n");
			std::ostringstream output;
			context.input_stream = &input;
			StreamRedirect redirect(std::cin, input.rdbuf(), std::cout, output.rdbuf());

			const int result = run_add(context, {"howdy-add", "alice"});

			bool ok = true;
			ok &= expect(result == 0, "interactive successful add returns 0");
			ok &= expect(context.preflight_calls == 1, "interactive add calls preflight");
			ok &= expect(context.preflight_saw_unread_input,
			             "preflight runs before label input is consumed");
			ok &= expect(context.capture_calls == 1, "interactive add calls capture");
			ok &= expect(context.capture_label == "front-door", "capture receives entered label");
			ok &= expect(context.append_calls == 1, "interactive add calls append");
			ok &= expect(context.appended_entry.label == "front-door",
			             "append receives entered label");
			ok &= expect(output.str().contains("Enter a label for this new model [automatic]: "),
			             "interactive add prompts for label");
			ok &= expect((context.events ==
			              std::vector<std::string>{"load", "preflight", "capture", "append"}),
			             "interactive add orders callbacks");
			return ok;
		}

		auto explicit_empty_label_still_prompts() -> bool {
			auto               context = make_success_context();
			std::istringstream input("front-door\n");
			std::ostringstream output;
			context.input_stream = &input;
			StreamRedirect redirect(std::cin, input.rdbuf(), std::cout, output.rdbuf());

			const int result = run_add(context, {"howdy-add", "alice", ""});

			bool ok = true;
			ok &= expect(result == 0, "explicit empty label add returns 0");
			ok &= expect(output.str().contains("Enter a label for this new model [automatic]: "),
			             "explicit empty label still prompts for a label");
			ok &= expect(context.capture_label == "front-door",
			             "explicit empty label uses entered label for capture");
			ok &= expect(context.appended_entry.label == "front-door",
			             "explicit empty label uses entered label for append");
			return ok;
		}

		auto plain_mode_skips_label_prompt() -> bool {
			auto               context = make_success_context();
			std::istringstream input("front-door\n");
			std::ostringstream output;
			context.input_stream = &input;
			StreamRedirect redirect(std::cin, input.rdbuf(), std::cout, output.rdbuf());

			const int result = run_add(context, {"howdy-add", "alice", "--plain"});

			bool ok = true;
			ok &= expect(result == 0, "plain add returns 0");
			ok &= expect(context.capture_plain, "plain flag reaches capture callback");
			ok &= expect(context.capture_label.empty(), "plain mode leaves automatic label empty");
			ok &=
			    expect(context.appended_entry.label.empty(), "plain mode appends automatic label");
			ok &=
			    expect(input.tellg() == std::streampos(0), "plain mode leaves label input unread");
			ok &= expect(!output.str().contains("Enter a label for this new model"),
			             "plain mode skips label prompt");
			return ok;
		}

		auto yes_flag_skips_label_prompt() -> bool {
			auto               context = make_success_context();
			std::istringstream input("front-door\n");
			std::ostringstream output;
			context.input_stream = &input;
			StreamRedirect redirect(std::cin, input.rdbuf(), std::cout, output.rdbuf());

			const int result = run_add(context, {"howdy-add", "alice", "-y"});

			bool ok = true;
			ok &= expect(result == 0, "yes add returns 0");
			ok &= expect(!context.capture_plain, "yes flag does not imply plain mode");
			ok &= expect(context.capture_label.empty(), "yes mode leaves automatic label empty");
			ok &= expect(context.appended_entry.label.empty(), "yes mode appends automatic label");
			ok &= expect(input.tellg() == std::streampos(0), "yes mode leaves label input unread");
			ok &= expect(!output.str().contains("Enter a label for this new model"),
			             "yes mode skips label prompt");
			return ok;
		}

		auto long_yes_argument_is_rejected() -> bool {
			auto context = make_success_context();

			const int result = run_add(context, {"howdy-add", "alice", "--yes"});

			bool ok = true;
			ok &= expect(result == 1, "unknown long yes argument returns 1");
			ok &= expect(context.load_calls == 0, "unknown long yes argument skips config load");
			ok &= expect(context.capture_calls == 0, "unknown long yes argument skips capture");
			ok &= expect(context.append_calls == 0, "unknown long yes argument skips append");
			return ok;
		}

		auto command_label_preserves_csv_characters() -> bool {
			auto context = make_success_context();

			const int result = run_add(context, {"howdy-add", "alice", "front,\"door"});

			bool ok = true;
			ok &= expect(result == 0, "CSV-character label add returns 0");
			ok &= expect(context.capture_label == "front,\"door",
			             "capture preserves comma and quote in label");
			ok &= expect(context.appended_entry.label == "front,\"door",
			             "append preserves comma and quote in label");
			return ok;
		}

		auto invalid_label_stops_before_capture() -> bool {
			auto context = make_success_context();

			const int result = run_add(context, {"howdy-add", "alice", "bad/name"});

			bool ok = true;
			ok &= expect(result == 1, "invalid label returns 1");
			ok &= expect(context.load_calls == 0, "invalid label skips config load");
			ok &= expect(context.preflight_calls == 0, "invalid label skips preflight");
			ok &= expect(context.capture_calls == 0, "invalid label skips camera capture");
			ok &= expect(context.append_calls == 0, "invalid label skips storage mutation");
			return ok;
		}

		auto interactive_label_truncates_to_24_characters() -> bool {
			auto               context = make_success_context();
			std::istringstream input("abcdefghijklmnopqrstuvwxyz\n");
			std::ostringstream output;
			StreamRedirect     redirect(std::cin, input.rdbuf(), std::cout, output.rdbuf());

			const int result = run_add(context, {"howdy-add", "alice"});

			bool ok = true;
			ok &= expect(result == 0, "long interactive label add returns 0");
			ok &= expect(context.capture_label == "abcdefghijklmnopqrstuvwx",
			             "capture receives truncated interactive label");
			ok &= expect(context.appended_entry.label == "abcdefghijklmnopqrstuvwx",
			             "append receives truncated interactive label");
			return ok;
		}

		auto append_failure_returns_error() -> bool {
			auto context          = make_success_context();
			context.append_result = howdy::native::UserModelMutationResult{
			    .status        = howdy::native::UserModelStatus::kWriteFailed,
			    .error_message = "append failed",
			};

			const int result = run_add(context, {"howdy-add", "alice", "front-door"});

			bool ok = true;
			ok &= expect(result == 1, "append failure returns 1");
			ok &= expect(context.preflight_calls == 1, "append failure calls preflight callback");
			ok &= expect(context.capture_calls == 1, "append failure calls capture callback");
			ok &= expect(context.append_calls == 1, "append failure calls append callback");
			return ok;
		}

	}  // namespace

	auto run_add_cli_success_tests() -> bool {
		bool ok = true;
		ok &= successful_enrollment_appends_expected_model();
		ok &= successful_interactive_flow_prompts_after_preflight();
		return ok;
	}

	auto run_add_cli_argument_tests() -> bool {
		bool ok = true;
		ok &= explicit_empty_label_still_prompts();
		ok &= plain_mode_skips_label_prompt();
		ok &= yes_flag_skips_label_prompt();
		ok &= long_yes_argument_is_rejected();
		ok &= command_label_preserves_csv_characters();
		ok &= invalid_label_stops_before_capture();
		ok &= interactive_label_truncates_to_24_characters();
		ok &= append_failure_returns_error();
		return ok;
	}

}  // namespace howdy::test::add_cli
