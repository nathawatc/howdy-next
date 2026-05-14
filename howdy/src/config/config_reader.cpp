#include "config/config_reader.hpp"

#include <utility>

namespace howdy::native {

ConfigReader::ConfigReader(std::string path)
    : path_(std::move(path)), reader_(path_) {}

auto ConfigReader::ok() const -> bool { return reader_.ParseError() == 0; }

auto ConfigReader::parse_error() const -> int { return reader_.ParseError(); }

auto ConfigReader::get(const std::string &section, const std::string &name,
                       const std::string &fallback) const -> std::string {
  return reader_.Get(section, name, fallback);
}

auto ConfigReader::get_int(const std::string &section, const std::string &name,
                           int fallback) const -> int {
  return static_cast<int>(reader_.GetInteger(section, name, fallback));
}

auto ConfigReader::get_float(const std::string &section, const std::string &name,
                             float fallback) const -> float {
  return static_cast<float>(reader_.GetReal(section, name, fallback));
}

auto ConfigReader::get_bool(const std::string &section, const std::string &name,
                            bool fallback) const -> bool {
  return reader_.GetBoolean(section, name, fallback);
}

}  // namespace howdy::native
