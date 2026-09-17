#pragma once

#include <sstream>
#include <string>
#include <string_view>

// The collector's own diagnostics (stderr). Not to be confused with the
// container logs it ships.
namespace podlogs::log {

enum class Level { Debug = 0, Info = 1, Warn = 2, Error = 3 };

void set_level(Level level);
Level level();
bool parse_level(std::string_view name, Level& out);
const char* level_name(Level level);
void write(Level level, const std::string& msg);

class Line {
 public:
  explicit Line(Level l) : level_(l), enabled_(l >= level()) {}
  ~Line() {
    if (enabled_) write(level_, ss_.str());
  }
  Line(const Line&) = delete;
  Line& operator=(const Line&) = delete;
  template <class T>
  Line& operator<<(const T& v) {
    if (enabled_) ss_ << v;
    return *this;
  }

 private:
  Level level_;
  bool enabled_;
  std::ostringstream ss_;
};

}  // namespace podlogs::log

#define LOG_DEBUG ::podlogs::log::Line(::podlogs::log::Level::Debug)
#define LOG_INFO ::podlogs::log::Line(::podlogs::log::Level::Info)
#define LOG_WARN ::podlogs::log::Line(::podlogs::log::Level::Warn)
#define LOG_ERROR ::podlogs::log::Line(::podlogs::log::Level::Error)
