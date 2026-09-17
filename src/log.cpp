#include "log.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>

#include "util.hpp"

namespace podlogs::log {

namespace {
std::atomic<int> g_level{static_cast<int>(Level::Info)};
std::mutex g_mutex;
}  // namespace

void set_level(Level level) { g_level.store(static_cast<int>(level)); }
Level level() { return static_cast<Level>(g_level.load()); }

bool parse_level(std::string_view name, Level& out) {
  const std::string n = util::to_lower(name);
  if (n == "debug") out = Level::Debug;
  else if (n == "info") out = Level::Info;
  else if (n == "warn" || n == "warning") out = Level::Warn;
  else if (n == "error") out = Level::Error;
  else return false;
  return true;
}

const char* level_name(Level level) {
  switch (level) {
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO";
    case Level::Warn: return "WARN";
    case Level::Error: return "ERROR";
  }
  return "?";
}

void write(Level level, const std::string& msg) {
  std::string ts = util::format_rfc3339(util::now_nanos());
  ts.erase(23);  // keep milliseconds
  ts += 'Z';
  std::lock_guard<std::mutex> lk(g_mutex);
  std::fprintf(stderr, "%s %-5s %s\n", ts.c_str(), level_name(level), msg.c_str());
  std::fflush(stderr);
}

}  // namespace podlogs::log
