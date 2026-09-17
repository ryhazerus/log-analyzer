#pragma once

#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

#include "demux.hpp"

namespace podlogs {

struct MultilineConfig {
  bool enabled = true;
  int flush_timeout_ms = 300;
  int max_lines = 500;
  size_t max_bytes = 256 * 1024;
  // ECMAScript regexes; a line matching any of them (or starting with
  // whitespace) is appended to the previous entry instead of starting a new one.
  std::vector<std::string> continuation_patterns = default_patterns();

  static std::vector<std::string> default_patterns();
};

struct ParseConfig {
  // JSON keys checked (in order) for the level / logger. Dotted keys walk
  // nested objects, so "log.level" matches ECS-style {"log":{"level":"info"}}.
  std::vector<std::string> level_keys = {"level", "lvl", "severity", "log.level", "loglevel",
                                         "levelname", "@level", "level_name"};
  std::vector<std::string> logger_keys = {"logger", "logger_name", "log.logger", "component"};
  // "pino" (10..60) or "python" (10..50) numeric level scheme.
  std::string numeric_levels = "pino";
  bool detect_text_levels = true;
  size_t text_scan_bytes = 256;
  MultilineConfig multiline;
};

// Maps the many spellings (WARNING, warn, Err, SEVERE, critical...) onto
// trace|debug|info|warn|error|fatal. Empty if unrecognised.
std::string normalize_level(std::string_view raw);
std::string level_from_number(double n, const std::string& scheme);

struct ParsedLine {
  bool is_json = false;
  std::string level;
  std::string logger;
};

class LineParser {
 public:
  explicit LineParser(const ParseConfig& cfg);

  ParsedLine parse(std::string_view line) const;
  bool is_continuation(std::string_view line) const;
  const ParseConfig& config() const { return cfg_; }

 private:
  std::string detect_text_level(std::string_view line) const;

  ParseConfig cfg_;
  std::vector<std::regex> patterns_;
};

// Joins stack traces and other continuation lines to the entry they belong
// to. One instance per (container, stream).
class MultilineAggregator {
 public:
  explicit MultilineAggregator(const LineParser& parser);

  void push(RawLine line, std::vector<RawLine>& out);
  void flush(std::vector<RawLine>& out);
  // Flushes the pending entry if nothing was appended for flush_timeout_ms.
  void flush_if_stale(util::Nanos now_wall, std::vector<RawLine>& out);
  bool has_pending() const { return pending_.has_value(); }

 private:
  const LineParser& parser_;
  std::optional<RawLine> pending_;
  int lines_ = 0;
  util::Nanos last_append_wall_ = 0;
};

}  // namespace podlogs
