#include "model_assets/opencv_model_manifest.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#ifndef HOWDY_SOURCE_DIR
#	error "HOWDY_SOURCE_DIR must be defined by CMake"
#endif

namespace {

	using howdy::native::OpenCvModelDescriptor;
	using howdy::test::expect;

	constexpr std::string_view kOpenCvZooRawPrefix  = "https://github.com/opencv/opencv_zoo/raw/";
	constexpr std::string_view kOpenCvZooTreePrefix = "https://github.com/opencv/opencv_zoo/tree/";

	struct OpenCvZooUrlParts {
		std::string_view revision;
		std::string_view model_directory;
	};

	struct NoticeSectionLookup {
		std::string_view section;
		std::size_t      matches{};
	};

	struct ProvenanceCheck {
		bool        ok;
		std::string error;
	};

	constexpr OpenCvModelDescriptor kFixtureModel{
	    .type     = howdy::native::OpenCvModelType::kYunet,
	    .filename = "fixture.onnx",
	    .url      = "https://github.com/opencv/opencv_zoo/raw/"
	                "0123456789abcdef0123456789abcdef01234567/"
	                "models/fixture/fixture.onnx",
	    .sha256   = "fixture-sha256",
	    .size     = 1,
	};

	constexpr auto kMatchingFixtureNotice = R"(# Notices

## Fixture Model

Artifact:

- `fixture.onnx`
- SHA-256: `fixture-sha256`

Source:

- OpenCV Zoo, revision `0123456789abcdef0123456789abcdef01234567`
- <https://github.com/opencv/opencv_zoo/tree/0123456789abcdef0123456789abcdef01234567/models/fixture>
)";

	constexpr auto kWrongShaFixtureNotice = R"(# Notices

## Other Model

- SHA-256: `fixture-sha256`

## Fixture Model

Artifact:

- `fixture.onnx`
- SHA-256: `wrong-sha`

Source:

- OpenCV Zoo, revision `0123456789abcdef0123456789abcdef01234567`
- <https://github.com/opencv/opencv_zoo/tree/0123456789abcdef0123456789abcdef01234567/models/fixture>
)";

	constexpr auto kWrongRevisionFixtureNotice = R"(# Notices

## Fixture Model

Artifact:

- `fixture.onnx`
- SHA-256: `fixture-sha256`

Source:

- OpenCV Zoo, revision `fedcba9876543210fedcba9876543210fedcba98`
- <https://github.com/opencv/opencv_zoo/tree/fedcba9876543210fedcba9876543210fedcba98/models/fixture>
)";

	auto IsHexDigit(const char value) -> bool {
		return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
		       (value >= 'A' && value <= 'F');
	}

	auto ParseOpencvZooUrl(const OpenCvModelDescriptor &model) -> std::optional<OpenCvZooUrlParts> {
		if (!model.url.starts_with(kOpenCvZooRawPrefix)) {
			return std::nullopt;
		}

		const auto raw_path     = model.url.substr(kOpenCvZooRawPrefix.size());
		const auto revision_end = raw_path.find('/');
		if (revision_end == std::string_view::npos) {
			return std::nullopt;
		}

		const auto revision = raw_path.substr(0, revision_end);
		if (revision.size() != 40) {
			return std::nullopt;
		}
		for (const char value : revision) {
			if (!IsHexDigit(value)) {
				return std::nullopt;
			}
		}

		const auto repository_path = raw_path.substr(revision_end + 1);
		const auto filename_start  = repository_path.rfind('/');
		if (filename_start == std::string_view::npos || filename_start == 0 ||
		    repository_path.substr(filename_start + 1) != model.filename) {
			return std::nullopt;
		}

		return OpenCvZooUrlParts{
		    .revision        = revision,
		    .model_directory = repository_path.substr(0, filename_start),
		};
	}

	auto Backticked(std::string_view value) -> std::string {
		std::string result{"`"};
		result += value;
		result += '`';
		return result;
	}

	auto FindNoticeSection(std::string_view notice, const OpenCvModelDescriptor &model)
	    -> NoticeSectionLookup {
		NoticeSectionLookup result{};
		const auto          filename_marker = Backticked(model.filename);
		auto section_start = notice.starts_with("## ") ? std::size_t{0} : notice.find("\n## ");

		while (section_start != std::string_view::npos) {
			if (section_start != 0) {
				++section_start;
			}
			const auto section_end = notice.find("\n## ", section_start + 3);
			const auto section = notice.substr(section_start, section_end == std::string_view::npos
			                                                      ? std::string_view::npos
			                                                      : section_end - section_start);
			if (section.contains(filename_marker)) {
				++result.matches;
				result.section = section;
			}
			if (section_end == std::string_view::npos) {
				break;
			}
			section_start = section_end;
		}
		return result;
	}

	auto Failure(std::string message) -> ProvenanceCheck {
		return {.ok = false, .error = std::move(message)};
	}

	auto CheckModelProvenance(std::string_view notice, const OpenCvModelDescriptor &model)
	    -> ProvenanceCheck {
		const auto url_parts = ParseOpencvZooUrl(model);
		if (!url_parts.has_value()) {
			return Failure("official OpenCV Zoo URL malformed for: " + std::string(model.filename));
		}

		const auto lookup = FindNoticeSection(notice, model);
		if (lookup.matches == 0) {
			return Failure("artifact section not found for: " + std::string(model.filename));
		}
		if (lookup.matches != 1) {
			return Failure("artifact filename identifies multiple notice sections for: " +
			               std::string(model.filename));
		}

		std::string sha_field{"SHA-256: "};
		sha_field += Backticked(model.sha256);
		if (!lookup.section.contains(sha_field)) {
			return Failure("notice SHA-256 does not match manifest for: " +
			               std::string(model.filename));
		}

		std::string revision_field{"OpenCV Zoo, revision "};
		revision_field += Backticked(url_parts->revision);
		if (!lookup.section.contains(revision_field)) {
			return Failure("notice OpenCV Zoo revision does not match manifest for: " +
			               std::string(model.filename));
		}

		std::string source_url{kOpenCvZooTreePrefix};
		source_url += url_parts->revision;
		source_url += '/';
		source_url += url_parts->model_directory;
		std::string source_link{"<"};
		source_link += source_url;
		source_link += '>';
		if (!lookup.section.contains(source_link)) {
			return Failure("notice source URL does not match manifest for: " +
			               std::string(model.filename));
		}

		return {.ok = true, .error = {}};
	}

	auto ReadTextFile(const std::filesystem::path &path) -> std::optional<std::string> {
		std::ifstream input(path, std::ios::binary);
		if (!input.is_open()) {
			return std::nullopt;
		}
		std::string contents{std::istreambuf_iterator<char>{input},
		                     std::istreambuf_iterator<char>{}};
		if (input.bad()) {
			return std::nullopt;
		}
		return contents;
	}

	auto TestFixtureChecks() -> bool {
		bool ok = true;

		const auto matching = CheckModelProvenance(kMatchingFixtureNotice, kFixtureModel);
		ok &= expect(matching.ok, "matching provenance fixture passes");

		const auto wrong_sha = CheckModelProvenance(kWrongShaFixtureNotice, kFixtureModel);
		ok &= expect(!wrong_sha.ok && wrong_sha.error.contains("SHA-256"),
		             "wrong SHA fails within artifact section");

		const auto wrong_revision =
		    CheckModelProvenance(kWrongRevisionFixtureNotice, kFixtureModel);
		ok &= expect(!wrong_revision.ok && wrong_revision.error.contains("revision"),
		             "wrong revision fails within artifact section");

		const auto malformed_model = OpenCvModelDescriptor{
		    .type     = kFixtureModel.type,
		    .filename = kFixtureModel.filename,
		    .url      = "https://github.com/opencv/opencv_zoo/raw/not-a-revision/"
		                "models/fixture/fixture.onnx",
		    .sha256   = kFixtureModel.sha256,
		    .size     = kFixtureModel.size,
		};
		const auto malformed = CheckModelProvenance(kMatchingFixtureNotice, malformed_model);
		ok &= expect(!malformed.ok && malformed.error.contains("URL malformed"),
		             "malformed OpenCV Zoo URL is rejected");

		return ok;
	}

}  // namespace

auto main() -> int {
	bool ok = TestFixtureChecks();

	const auto notice_path = std::filesystem::path(HOWDY_SOURCE_DIR) / "THIRD_PARTY_NOTICES.md";
	const auto notice      = ReadTextFile(notice_path);
	if (!notice.has_value()) {
		return expect(false, "read third-party notice: " + notice_path.string()) ? 0 : 1;
	}

	for (const auto &model : howdy::native::OfficialOpencvModels()) {
		const auto result = CheckModelProvenance(*notice, model);
		ok &= expect(result.ok, result.error);
	}

	return ok ? 0 : 1;
}
