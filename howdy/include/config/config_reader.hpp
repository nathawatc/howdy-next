#pragma once

#include <INIReader.h>
#include <string>
#include <vector>

namespace howdy::native {

	class ConfigReader {
	public:
		explicit ConfigReader(std::string path);

		[[nodiscard]] auto ok() const -> bool;
		[[nodiscard]] auto parse_error() const -> int;
		[[nodiscard]] auto get(const std::string &section, const std::string &name,
		                       const std::string &fallback) const -> std::string;
		[[nodiscard]] auto get_int(const std::string &section, const std::string &name,
		                           int fallback) const -> int;
		[[nodiscard]] auto get_float(const std::string &section, const std::string &name,
		                             float fallback) const -> float;
		[[nodiscard]] auto get_bool(const std::string &section, const std::string &name,
		                            bool fallback) const -> bool;
		[[nodiscard]] auto sections() const -> std::vector<std::string>;
		[[nodiscard]] auto keys(const std::string &section) const -> std::vector<std::string>;
		[[nodiscard]] auto has_value(const std::string &section, const std::string &name) const
		    -> bool;

	private:
		std::string path_;
		INIReader   reader_;
	};

}  // namespace howdy::native
