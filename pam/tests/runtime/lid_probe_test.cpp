#include "runtime/lid_probe.hpp"
#include "test_support.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

	using howdy::pam::runtime::LidProbeStatus;
	using howdy::pam::runtime::LidState;
	using howdy::pam::runtime::ReadLidStateFromPattern;
	using howdy::test::Expect;

	auto WriteState(const std::filesystem::path &path, std::string_view state) -> bool {
		std::ofstream output(path);
		output << state << '\n';
		return output.good();
	}

	auto PatternFor(const std::filesystem::path &scenario) -> std::string {
		return (scenario / "*" / "state").string();
	}

	auto WriteScenarioState(const std::filesystem::path &scenario, const char *name,
	                        std::string_view state) -> bool {
		const auto lid_directory = scenario / name;
		return std::filesystem::create_directories(lid_directory) &&
		       WriteState(lid_directory / "state", state);
	}

	auto CreateUnreadableScenarioState(const std::filesystem::path &scenario, const char *name)
	    -> bool {
		return std::filesystem::create_directories(scenario / name / "state");
	}

}  // namespace

auto main() -> int {
	bool ok = true;

	const auto root = std::filesystem::temp_directory_path() /
	                  ("howdy-lid-probe-" + std::to_string(getpid()) + "-" +
	                   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::error_code error;
	std::filesystem::remove_all(root, error);
	ok &= Expect(std::filesystem::create_directories(root), "creates lid probe fixture root");
	if (!ok) {
		return 1;
	}

	const auto no_lid = root / "no-lid";
	ok &= Expect(std::filesystem::create_directories(no_lid), "creates no-match lid scenario");
	const auto no_lid_result = ReadLidStateFromPattern(PatternFor(no_lid));
	ok &= Expect(no_lid_result.status == LidProbeStatus::kOk &&
	                 no_lid_result.state == LidState::kUnknown,
	             "no matching lid states are normal unknown result");

	const auto open_only = root / "open-only";
	ok &= Expect(WriteScenarioState(open_only, "lid0", "state:      open"),
	             "writes open-only lid state");
	const auto open_only_result = ReadLidStateFromPattern(PatternFor(open_only));
	ok &= Expect(open_only_result.status == LidProbeStatus::kOk &&
	                 open_only_result.state == LidState::kOpen,
	             "readable open lid state is reported open");

	const auto closed_only = root / "closed-only";
	ok &= Expect(WriteScenarioState(closed_only, "lid0", "state:      closed"),
	             "writes closed-only lid state");
	const auto closed_only_result = ReadLidStateFromPattern(PatternFor(closed_only));
	ok &= Expect(closed_only_result.status == LidProbeStatus::kOk &&
	                 closed_only_result.state == LidState::kClosed,
	             "readable closed lid state is reported closed");

	const auto invalid_pattern = ReadLidStateFromPattern("");
	ok &= Expect(invalid_pattern.status == LidProbeStatus::kError &&
	                 invalid_pattern.state == LidState::kUnknown,
	             "invalid lid pattern is reported as probe error with unknown state");

	const auto unreadable = root / "unreadable";
	ok &= Expect(CreateUnreadableScenarioState(unreadable, "lid0"),
	             "creates deterministic unreadable lid fixture");
	const auto unreadable_result = ReadLidStateFromPattern(PatternFor(unreadable));
	ok &= Expect(unreadable_result.status == LidProbeStatus::kError &&
	                 unreadable_result.state == LidState::kUnknown &&
	                 !unreadable_result.error_message.empty(),
	             "unopenable lid state reports error, unknown state and diagnostic");
	ok &= Expect(unreadable_result.error_message.contains((unreadable / "lid0" / "state").string()),
	             "unopenable lid diagnostic identifies affected path");

	const auto unrecognized = root / "unrecognized";
	ok &= Expect(WriteScenarioState(unrecognized, "lid0", "state:      unknown"),
	             "writes unrecognized lid content");
	const auto unrecognized_result = ReadLidStateFromPattern(PatternFor(unrecognized));
	ok &= Expect(unrecognized_result.status == LidProbeStatus::kOk &&
	                 unrecognized_result.state == LidState::kUnknown,
	             "unrecognized readable lid content remains unknown-success");

	const auto unreadable_open = root / "unreadable-open";
	ok &= Expect(CreateUnreadableScenarioState(unreadable_open, "a-unreadable"),
	             "creates unreadable-open error source");
	ok &= Expect(WriteScenarioState(unreadable_open, "z-open", "state:      open"),
	             "creates unreadable-open valid source");
	const auto unreadable_open_result = ReadLidStateFromPattern(PatternFor(unreadable_open));
	ok &= Expect(unreadable_open_result.status == LidProbeStatus::kError &&
	                 unreadable_open_result.state == LidState::kOpen &&
	                 !unreadable_open_result.error_message.empty(),
	             "unreadable plus open keeps open state and non-fatal diagnostic");
	ok &= Expect(unreadable_open_result.error_message.contains("a-unreadable/state"),
	             "unreadable plus open diagnostic identifies error source");

	const auto unreadable_closed = root / "unreadable-closed";
	ok &= Expect(CreateUnreadableScenarioState(unreadable_closed, "a-unreadable"),
	             "creates unreadable-closed error source");
	ok &= Expect(WriteScenarioState(unreadable_closed, "z-closed", "state:      closed"),
	             "creates unreadable-closed valid source");
	const auto unreadable_closed_result = ReadLidStateFromPattern(PatternFor(unreadable_closed));
	ok &= Expect(unreadable_closed_result.status == LidProbeStatus::kError &&
	                 unreadable_closed_result.state == LidState::kClosed &&
	                 !unreadable_closed_result.error_message.empty(),
	             "unreadable plus closed keeps closed state and diagnostic");
	ok &= Expect(unreadable_closed_result.error_message.contains("a-unreadable/state"),
	             "unreadable plus closed diagnostic identifies error source");

	const auto open_open = root / "open-open";
	ok &= Expect(WriteScenarioState(open_open, "a-open", "state:      open"),
	             "creates first open source");
	ok &= Expect(WriteScenarioState(open_open, "z-open", "state:      open"),
	             "creates second open source");
	ok &= Expect(ReadLidStateFromPattern(PatternFor(open_open)).state == LidState::kOpen,
	             "open plus open aggregates as open");

	const auto open_closed = root / "open-closed";
	ok &= Expect(WriteScenarioState(open_closed, "a-open", "state:      open"),
	             "creates open source before closed source");
	ok &= Expect(WriteScenarioState(open_closed, "z-closed", "state:      closed"),
	             "creates closed source after open source");
	ok &= Expect(ReadLidStateFromPattern(PatternFor(open_closed)).state == LidState::kClosed,
	             "open plus closed aggregates conservatively as closed");

	const auto closed_open = root / "closed-open";
	ok &= Expect(WriteScenarioState(closed_open, "a-closed", "state:      closed"),
	             "creates closed source before open source");
	ok &= Expect(WriteScenarioState(closed_open, "z-open", "state:      open"),
	             "creates open source after closed source");
	ok &= Expect(ReadLidStateFromPattern(PatternFor(closed_open)).state == LidState::kClosed,
	             "closed plus open remains closed independent of glob order");

	const auto closed_closed = root / "closed-closed";
	ok &= Expect(WriteScenarioState(closed_closed, "a-closed", "state:      closed"),
	             "creates first closed source");
	ok &= Expect(WriteScenarioState(closed_closed, "z-closed", "state:      closed"),
	             "creates second closed source");
	ok &= Expect(ReadLidStateFromPattern(PatternFor(closed_closed)).state == LidState::kClosed,
	             "closed plus closed aggregates as closed");

	std::filesystem::remove_all(root, error);
	return ok ? 0 : 1;
}
