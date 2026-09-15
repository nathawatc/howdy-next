#include "cli/add.hpp"
#include "cli/add_cli_test_support.hpp"
#include "vision/face_model.hpp"

#include <array>
#include <iostream>
#include <sstream>

namespace howdy::test::add_cli {

	namespace {

		auto PublicMissingUserReturnsError() -> bool {
			auto                  command = std::to_array("howdy-add");
			std::array<char *, 1> argv{command.data()};
			return Expect(AddMain(1, argv.data()) == 1,
			              "add entrypoint returns on invalid arguments");
		}

		auto SuccessfulEnrollmentAppendsExpectedModel() -> bool {
			auto      context = MakeSuccessContext();
			const int result  = RunAdd(context, {"howdy-add", "alice", "front-door"});

			bool ok = true;
			ok &= Expect(result == 0, "successful add returns 0");
			ok &= Expect(context.load_calls == 1, "runtime config loaded once");
			ok &= Expect(context.preflight_calls == 1, "preflight callback called once");
			ok &= Expect(context.preflight_user == "alice", "preflight callback receives user");
			ok &= Expect(context.preflight_config.video.dark_threshold == 32.0F,
			             "preflight callback receives runtime config");
			ok &= Expect(context.capture_calls == 1, "capture callback called once");
			ok &= Expect(!context.capture_plain, "non-plain flag reaches capture callback");
			ok &= Expect(context.capture_user == "alice", "capture callback receives user");
			ok &= Expect(context.capture_config.video.dark_threshold == 32.0F,
			             "capture callback receives runtime config");
			ok &= Expect(context.capture_label == "front-door", "capture callback receives label");
			ok &= Expect(context.append_calls == 1, "append callback called once");
			ok &= Expect(context.appended_user == "alice", "append callback receives user");
			ok &= Expect(context.appended_entry.label == "front-door",
			             "append callback receives label");
			ok &= Expect(context.appended_entry.backend == howdy::native::FaceModel::kBackendName,
			             "append callback receives backend");
			ok &= Expect(context.appended_entry.metric == howdy::native::FaceMetric::kCosine,
			             "append callback receives metric");
			ok &= Expect(context.appended_entry.model == howdy::native::FaceModel::kSfaceModel,
			             "append callback receives model");
			ok &= Expect(context.appended_entry.encodings.size() == 1,
			             "append callback receives one encoding vector");
			ok &= Expect(context.appended_entry.encodings.front().size() == 1,
			             "append callback receives one encoding value");
			ok &= Expect(context.appended_entry.encodings.front().front() == 0.125F,
			             "append callback receives encoding");
			ok &= Expect((context.events ==
			              std::vector<std::string>{"load", "preflight", "capture", "append"}),
			             "successful add orders callbacks");
			return ok;
		}

		auto SuccessfulInteractiveFlowPromptsAfterPreflight() -> bool {
			auto               context = MakeSuccessContext();
			std::istringstream input("front-door\n");
			std::ostringstream output;
			context.input_stream = &input;
			StreamRedirect redirect(std::cin, input.rdbuf(), std::cout, output.rdbuf());

			const int result = RunAdd(context, {"howdy-add", "alice"});

			bool ok = true;
			ok &= Expect(result == 0, "interactive successful add returns 0");
			ok &= Expect(context.preflight_calls == 1, "interactive add calls preflight");
			ok &= Expect(context.preflight_saw_unread_input,
			             "preflight runs before label input is consumed");
			ok &= Expect(context.capture_calls == 1, "interactive add calls capture");
			ok &= Expect(context.capture_label == "front-door", "capture receives entered label");
			ok &= Expect(context.append_calls == 1, "interactive add calls append");
			ok &= Expect(context.appended_entry.label == "front-door",
			             "append receives entered label");
			ok &= Expect(output.str().contains("Enter a label for this new model [automatic]: "),
			             "interactive add prompts for label");
			ok &= Expect((context.events ==
			              std::vector<std::string>{"load", "preflight", "capture", "append"}),
			             "interactive add orders callbacks");
			return ok;
		}

		auto ExplicitEmptyLabelStillPrompts() -> bool {
			auto               context = MakeSuccessContext();
			std::istringstream input("front-door\n");
			std::ostringstream output;
			context.input_stream = &input;
			StreamRedirect redirect(std::cin, input.rdbuf(), std::cout, output.rdbuf());

			const int result = RunAdd(context, {"howdy-add", "alice", ""});

			bool ok = true;
			ok &= Expect(result == 0, "explicit empty label add returns 0");
			ok &= Expect(output.str().contains("Enter a label for this new model [automatic]: "),
			             "explicit empty label still prompts for a label");
			ok &= Expect(context.capture_label == "front-door",
			             "explicit empty label uses entered label for capture");
			ok &= Expect(context.appended_entry.label == "front-door",
			             "explicit empty label uses entered label for append");
			return ok;
		}

		auto PlainModeSkipsLabelPrompt() -> bool {
			auto               context = MakeSuccessContext();
			std::istringstream input("front-door\n");
			std::ostringstream output;
			context.input_stream = &input;
			StreamRedirect redirect(std::cin, input.rdbuf(), std::cout, output.rdbuf());

			const int result = RunAdd(context, {"howdy-add", "alice", "--plain"});

			bool ok = true;
			ok &= Expect(result == 0, "plain add returns 0");
			ok &= Expect(context.capture_plain, "plain flag reaches capture callback");
			ok &= Expect(context.capture_label.empty(), "plain mode leaves automatic label empty");
			ok &=
			    Expect(context.appended_entry.label.empty(), "plain mode appends automatic label");
			ok &=
			    Expect(input.tellg() == std::streampos(0), "plain mode leaves label input unread");
			ok &= Expect(!output.str().contains("Enter a label for this new model"),
			             "plain mode skips label prompt");
			return ok;
		}

		auto YesFlagSkipsLabelPrompt() -> bool {
			auto               context = MakeSuccessContext();
			std::istringstream input("front-door\n");
			std::ostringstream output;
			context.input_stream = &input;
			StreamRedirect redirect(std::cin, input.rdbuf(), std::cout, output.rdbuf());

			const int result = RunAdd(context, {"howdy-add", "alice", "-y"});

			bool ok = true;
			ok &= Expect(result == 0, "yes add returns 0");
			ok &= Expect(!context.capture_plain, "yes flag does not imply plain mode");
			ok &= Expect(context.capture_label.empty(), "yes mode leaves automatic label empty");
			ok &= Expect(context.appended_entry.label.empty(), "yes mode appends automatic label");
			ok &= Expect(input.tellg() == std::streampos(0), "yes mode leaves label input unread");
			ok &= Expect(!output.str().contains("Enter a label for this new model"),
			             "yes mode skips label prompt");
			return ok;
		}

		auto LongYesArgumentIsRejected() -> bool {
			auto context = MakeSuccessContext();

			const int result = RunAdd(context, {"howdy-add", "alice", "--yes"});

			bool ok = true;
			ok &= Expect(result == 1, "unknown long yes argument returns 1");
			ok &= Expect(context.load_calls == 0, "unknown long yes argument skips config load");
			ok &= Expect(context.capture_calls == 0, "unknown long yes argument skips capture");
			ok &= Expect(context.append_calls == 0, "unknown long yes argument skips append");
			return ok;
		}

		auto CommandLabelPreservesCsvCharacters() -> bool {
			auto context = MakeSuccessContext();

			const int result = RunAdd(context, {"howdy-add", "alice", "front,\"door"});

			bool ok = true;
			ok &= Expect(result == 0, "CSV-character label add returns 0");
			ok &= Expect(context.capture_label == "front,\"door",
			             "capture preserves comma and quote in label");
			ok &= Expect(context.appended_entry.label == "front,\"door",
			             "append preserves comma and quote in label");
			return ok;
		}

		auto InvalidLabelStopsBeforeCapture() -> bool {
			auto context = MakeSuccessContext();

			const int result = RunAdd(context, {"howdy-add", "alice", "bad/name"});

			bool ok = true;
			ok &= Expect(result == 1, "invalid label returns 1");
			ok &= Expect(context.load_calls == 0, "invalid label skips config load");
			ok &= Expect(context.preflight_calls == 0, "invalid label skips preflight");
			ok &= Expect(context.capture_calls == 0, "invalid label skips camera capture");
			ok &= Expect(context.append_calls == 0, "invalid label skips storage mutation");
			return ok;
		}

		auto InteractiveLabelTruncatesTo24Characters() -> bool {
			auto               context = MakeSuccessContext();
			std::istringstream input("abcdefghijklmnopqrstuvwxyz\n");
			std::ostringstream output;
			StreamRedirect     redirect(std::cin, input.rdbuf(), std::cout, output.rdbuf());

			const int result = RunAdd(context, {"howdy-add", "alice"});

			bool ok = true;
			ok &= Expect(result == 0, "long interactive label add returns 0");
			ok &= Expect(context.capture_label == "abcdefghijklmnopqrstuvwx",
			             "capture receives truncated interactive label");
			ok &= Expect(context.appended_entry.label == "abcdefghijklmnopqrstuvwx",
			             "append receives truncated interactive label");
			return ok;
		}

		auto AppendFailureReturnsError() -> bool {
			auto context          = MakeSuccessContext();
			context.append_result = howdy::native::UserModelMutationResult{
			    .status        = howdy::native::UserModelStatus::kWriteFailed,
			    .error_message = "append failed",
			};

			const int result = RunAdd(context, {"howdy-add", "alice", "front-door"});

			bool ok = true;
			ok &= Expect(result == 1, "append failure returns 1");
			ok &= Expect(context.preflight_calls == 1, "append failure calls preflight callback");
			ok &= Expect(context.capture_calls == 1, "append failure calls capture callback");
			ok &= Expect(context.append_calls == 1, "append failure calls append callback");
			return ok;
		}

	}  // namespace

	auto RunAddCliSuccessTests() -> bool {
		bool ok = true;
		ok &= SuccessfulEnrollmentAppendsExpectedModel();
		ok &= SuccessfulInteractiveFlowPromptsAfterPreflight();
		return ok;
	}

	auto RunAddCliArgumentTests() -> bool {
		bool ok = true;
		ok &= PublicMissingUserReturnsError();
		ok &= ExplicitEmptyLabelStillPrompts();
		ok &= PlainModeSkipsLabelPrompt();
		ok &= YesFlagSkipsLabelPrompt();
		ok &= LongYesArgumentIsRejected();
		ok &= CommandLabelPreservesCsvCharacters();
		ok &= InvalidLabelStopsBeforeCapture();
		ok &= InteractiveLabelTruncatesTo24Characters();
		ok &= AppendFailureReturnsError();
		return ok;
	}

}  // namespace howdy::test::add_cli
