#include "parse.hpp"

#include <cctype>
#include <cmath>

#include "nlohmann/json.hpp"
#include "util.hpp"

namespace podlogs {

using nlohmann::json;

std::vector<std::string> MultilineConfig::default_patterns() {
  return {
      R"(^at\s+\S)",                                                       // Java/.NET frames
      R"(^Caused by:)",                                                    // Java causes
      R"(^\.\.\.\s*\d+\s+(more|common frames omitted))",                   // Java elided frames
      R"(^[A-Za-z_$][\w$]*(\.[A-Za-z_$][\w$]*)+(Exception|Error|Throwable)(:|$))",  // java.lang.FooException: msg
      R"(^[A-Za-z_]\w*(Error|Exception|Warning)(:|$))",                    // Python: ValueError: msg
      R"(^Traceback \(most recent call last\):)",                          // Python
      R"(^goroutine \d+ \[)",                                              // Go panics
      R"(^---> )",                                                         // .NET inner exceptions
  };
}

std::string normalize_level(std::string_view raw) {
  const std::string l = util::to_lower(util::trim(raw));
  if (l.empty()) return "";
  switch (l[0]) {
    case 't':
      if (l == "trace") return "trace";
      break;
    case 'd':
      if (l == "debug" || l == "dbg") return "debug";
      break;
    case 'f':
      if (l == "fatal") return "fatal";
      if (l == "fine") return "debug";
      if (l == "finer" || l == "finest") return "trace";
      break;
    case 'v':
      if (l == "verbose") return "debug";
      break;
    case 'i':
      if (l == "info" || l == "information" || l == "informational") return "info";
      break;
    case 'n':
      if (l == "notice") return "info";
      break;
    case 'w':
      if (l == "warn" || l == "warning") return "warn";
      break;
    case 'e':
      if (l == "error" || l == "err") return "error";
      if (l == "emerg" || l == "emergency") return "fatal";
      break;
    case 's':
      if (l == "severe") return "error";
      break;
    case 'c':
      if (l == "critical" || l == "crit") return "fatal";
      break;
    case 'p':
      if (l == "panic") return "fatal";
      break;
    case 'a':
      if (l == "alert") return "fatal";
      break;
    default:
      break;
  }
  return "";
}

std::string level_from_number(double n, const std::string& scheme) {
  if (std::isnan(n)) return "";
  if (scheme == "python") {
    if (n >= 50) return "fatal";
    if (n >= 40) return "error";
    if (n >= 30) return "warn";
    if (n >= 20) return "info";
    if (n >= 10) return "debug";
    return "trace";
  }
  // pino / bunyan
  if (n >= 60) return "fatal";
  if (n >= 50) return "error";
  if (n >= 40) return "warn";
  if (n >= 30) return "info";
  if (n >= 20) return "debug";
  return "trace";
}

namespace {

const json* find_key(const json& obj, const std::string& key) {
  if (auto it = obj.find(key); it != obj.end()) return &*it;
  if (key.find('.') == std::string::npos) return nullptr;
  const json* cur = &obj;
  for (const auto& part : util::split(key, '.')) {
    if (!cur->is_object()) return nullptr;
    auto it = cur->find(part);
    if (it == cur->end()) return nullptr;
    cur = &*it;
  }
  return cur;
}

bool is_word_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

}  // namespace

LineParser::LineParser(const ParseConfig& cfg) : cfg_(cfg) {
  for (const auto& p : cfg_.multiline.continuation_patterns) {
    patterns_.emplace_back(p, std::regex::ECMAScript | std::regex::optimize);
  }
}

ParsedLine LineParser::parse(std::string_view line) const {
  ParsedLine out;
  std::string_view body = util::trim(line);
  if (!body.empty() && body.front() == '{' && body.back() == '}') {
    const json j = json::parse(body, nullptr, false);
    if (j.is_object()) {
      out.is_json = true;
      for (const auto& key : cfg_.level_keys) {
        const json* v = find_key(j, key);
        if (!v) continue;
        if (v->is_string()) out.level = normalize_level(v->get_ref<const std::string&>());
        else if (v->is_number()) out.level = level_from_number(v->get<double>(), cfg_.numeric_levels);
        if (!out.level.empty()) break;
      }
      for (const auto& key : cfg_.logger_keys) {
        const json* v = find_key(j, key);
        if (v && v->is_string() && !v->get_ref<const std::string&>().empty()) {
          out.logger = v->get_ref<const std::string&>();
          break;
        }
      }
      return out;
    }
  }
  if (cfg_.detect_text_levels) out.level = detect_text_level(body);
  return out;
}

std::string LineParser::detect_text_level(std::string_view line) const {
  if (line.size() > cfg_.text_scan_bytes) line = line.substr(0, cfg_.text_scan_bytes);
  size_t i = 0;
  while (i < line.size()) {
    if (!is_word_char(line[i])) {
      ++i;
      continue;
    }
    const size_t start = i;
    bool alpha_only = true;
    while (i < line.size() && is_word_char(line[i])) {
      if (!std::isalpha(static_cast<unsigned char>(line[i]))) alpha_only = false;
      ++i;
    }
    const size_t len = i - start;
    if (!alpha_only || len < 3 || len > 13) continue;
    const std::string lvl = normalize_level(line.substr(start, len));
    if (!lvl.empty()) return lvl;
  }
  return "";
}

bool LineParser::is_continuation(std::string_view line) const {
  if (line.empty()) return false;
  if (line.front() == ' ' || line.front() == '\t') return true;
  if (line.front() == '{') return false;
  const std::string_view head = line.substr(0, std::min<size_t>(line.size(), 160));
  for (const auto& re : patterns_) {
    if (std::regex_search(head.begin(), head.end(), re)) return true;
  }
  return false;
}

// ---------------------------------------------------------------------------

MultilineAggregator::MultilineAggregator(const LineParser& parser) : parser_(parser) {}

void MultilineAggregator::push(RawLine line, std::vector<RawLine>& out) {
  const MultilineConfig& cfg = parser_.config().multiline;
  if (!cfg.enabled) {
    out.push_back(std::move(line));
    return;
  }
  const util::Nanos now = util::now_nanos();
  if (pending_ && lines_ < cfg.max_lines && pending_->text.size() + line.text.size() < cfg.max_bytes &&
      parser_.is_continuation(line.text)) {
    pending_->text.push_back('\n');
    pending_->text.append(line.text);
    if (line.ts > pending_->ts_last) pending_->ts_last = line.ts;
    ++lines_;
    last_append_wall_ = now;
    return;
  }
  flush(out);
  pending_ = std::move(line);
  if (pending_->ts_last < pending_->ts) pending_->ts_last = pending_->ts;
  lines_ = 1;
  last_append_wall_ = now;
}

void MultilineAggregator::flush(std::vector<RawLine>& out) {
  if (!pending_) return;
  out.push_back(std::move(*pending_));
  pending_.reset();
  lines_ = 0;
}

void MultilineAggregator::flush_if_stale(util::Nanos now_wall, std::vector<RawLine>& out) {
  if (!pending_) return;
  const util::Nanos timeout = static_cast<util::Nanos>(parser_.config().multiline.flush_timeout_ms) * util::kNanosPerMs;
  if (now_wall - last_append_wall_ >= timeout) flush(out);
}

}  // namespace podlogs
