#include "config_reader/internal.hpp"

#include <utility>

namespace howdy::native {

	ConfigReader::ConfigReader(std::string path)
	    : path_(std::move(path))
	    , reader_(path_) {}

	ConfigReader::ConfigReader(std::string path, std::string_view content)
	    : path_(std::move(path))
	    , reader_(content.data(), content.size()) {}

	auto ConfigReader::Ok() const -> bool {
		return reader_.ParseError() == 0;
	}

	auto ConfigReader::ParseError() const -> int {
		return reader_.ParseError();
	}

	auto ConfigReader::Get(const std::string &section, const std::string &name,
	                       const std::string &fallback) const -> std::string {
		return reader_.Get(section, name, fallback);
	}

	auto ConfigReader::GetInt(const std::string &section, const std::string &name,
	                          int fallback) const -> int {
		return static_cast<int>(reader_.GetInteger(section, name, fallback));
	}

	auto ConfigReader::GetFloat(const std::string &section, const std::string &name,
	                            float fallback) const -> float {
		const auto value = reader_.Get(section, name, "");
		if (const auto parsed = ParseConfigFloatStrict(value)) {
			return *parsed;
		}
		return fallback;
	}

	auto ConfigReader::GetBool(const std::string &section, const std::string &name,
	                           bool fallback) const -> bool {
		return reader_.GetBoolean(section, name, fallback);
	}

	auto ConfigReader::Sections() const -> std::vector<std::string> {
		return reader_.Sections();
	}

	auto ConfigReader::Keys(const std::string &section) const -> std::vector<std::string> {
		return reader_.Keys(section);
	}

	auto ConfigReader::HasValue(const std::string &section, const std::string &name) const -> bool {
		return reader_.HasValue(section, name);
	}

}  // namespace howdy::native
