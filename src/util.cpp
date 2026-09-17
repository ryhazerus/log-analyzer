#include "util.hpp"

#include <unistd.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace podlogs::util {

Nanos now_nanos() {
  using namespace std::chrono;
  return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
}

namespace {

bool parse_digits(std::string_view s, size_t pos, size_t len, int& out) {
  if (pos + len > s.size()) return false;
  int v = 0;
  for (size_t i = pos; i < pos + len; ++i) {
    char c = s[i];
    if (c < '0' || c > '9') return false;
    v = v * 10 + (c - '0');
  }
  out = v;
  return true;
}

}  // namespace

std::optional<Nanos> parse_rfc3339(std::string_view s) {
  // YYYY-MM-DD[T ]HH:MM:SS[.frac](Z|+HH:MM|-HHMM)
  if (s.size() < 20) return std::nullopt;
  int Y, M, D, h, m, sec;
  if (!parse_digits(s, 0, 4, Y) || s[4] != '-' || !parse_digits(s, 5, 2, M) || s[7] != '-' ||
      !parse_digits(s, 8, 2, D)) {
    return std::nullopt;
  }
  if (s[10] != 'T' && s[10] != 't' && s[10] != ' ') return std::nullopt;
  if (!parse_digits(s, 11, 2, h) || s[13] != ':' || !parse_digits(s, 14, 2, m) || s[16] != ':' ||
      !parse_digits(s, 17, 2, sec)) {
    return std::nullopt;
  }
  size_t pos = 19;
  Nanos frac = 0;
  if (pos < s.size() && (s[pos] == '.' || s[pos] == ',')) {
    ++pos;
    int digits = 0;
    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
      if (digits < 9) {
        frac = frac * 10 + (s[pos] - '0');
        ++digits;
      }
      ++pos;
    }
    if (digits == 0) return std::nullopt;
    for (; digits < 9; ++digits) frac *= 10;
  }
  if (pos >= s.size()) return std::nullopt;
  int offset_sec = 0;
  if (s[pos] == 'Z' || s[pos] == 'z') {
    ++pos;
  } else if (s[pos] == '+' || s[pos] == '-') {
    const int sign = s[pos] == '+' ? 1 : -1;
    ++pos;
    int oh, om;
    if (!parse_digits(s, pos, 2, oh)) return std::nullopt;
    pos += 2;
    if (pos < s.size() && s[pos] == ':') ++pos;
    if (!parse_digits(s, pos, 2, om)) return std::nullopt;
    pos += 2;
    offset_sec = sign * (oh * 3600 + om * 60);
  } else {
    return std::nullopt;
  }
  if (pos != s.size()) return std::nullopt;
  if (M < 1 || M > 12 || D < 1 || D > 31 || h > 23 || m > 59 || sec > 60) return std::nullopt;

  std::tm tm{};
  tm.tm_year = Y - 1900;
  tm.tm_mon = M - 1;
  tm.tm_mday = D;
  tm.tm_hour = h;
  tm.tm_min = m;
  tm.tm_sec = sec;
  const time_t t = timegm(&tm);
  return (static_cast<Nanos>(t) - offset_sec) * kNanosPerSec + frac;
}

std::string format_rfc3339(Nanos ns) {
  Nanos secs = ns / kNanosPerSec;
  Nanos frac = ns % kNanosPerSec;
  if (frac < 0) {
    frac += kNanosPerSec;
    secs -= 1;
  }
  const time_t t = static_cast<time_t>(secs);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[64];
  const int n = std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%09lldZ", tm.tm_year + 1900,
                              tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                              static_cast<long long>(frac));
  return std::string(buf, static_cast<size_t>(n));
}

std::optional<int64_t> parse_duration_ms(std::string_view in) {
  const std::string_view s = trim(in);
  if (s.empty()) return std::nullopt;
  size_t i = 0;
  while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.')) ++i;
  if (i == 0) return std::nullopt;
  const std::string num(s.substr(0, i));
  char* end = nullptr;
  const double v = std::strtod(num.c_str(), &end);
  if (end == num.c_str() || *end != '\0') return std::nullopt;
  const std::string_view unit = trim(s.substr(i));
  double mult;
  if (unit.empty() || unit == "s") mult = 1000;
  else if (unit == "ms") mult = 1;
  else if (unit == "m") mult = 60'000;
  else if (unit == "h") mult = 3'600'000;
  else if (unit == "d") mult = 86'400'000;
  else return std::nullopt;
  return static_cast<int64_t>(v * mult);
}

std::string hostname() {
  char buf[256];
  if (gethostname(buf, sizeof buf) != 0) return "unknown";
  buf[sizeof buf - 1] = '\0';
  return buf;
}

std::string to_lower(std::string_view s) {
  std::string out(s);
  for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string to_upper(std::string_view s) {
  std::string out(s);
  for (auto& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

bool starts_with(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}

std::vector<std::string> split(std::string_view s, char sep) {
  std::vector<std::string> out;
  size_t start = 0;
  while (true) {
    const size_t pos = s.find(sep, start);
    if (pos == std::string_view::npos) {
      out.emplace_back(s.substr(start));
      return out;
    }
    out.emplace_back(s.substr(start, pos - start));
    start = pos + 1;
  }
}

std::string short_id(std::string_view id, size_t n) {
  if (starts_with(id, "sha256:")) id.remove_prefix(7);
  return std::string(id.substr(0, n));
}

std::string sanitize_label_name(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 1);
  for (char c : s) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    out.push_back(ok ? c : '_');
  }
  if (out.empty()) out = "_";
  if (out[0] >= '0' && out[0] <= '9') out.insert(out.begin(), '_');
  return out;
}

std::string url_encode(std::string_view s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size() * 3);
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 15]);
    }
  }
  return out;
}

std::string getenv_or(const char* name, const std::string& def) {
  const char* v = std::getenv(name);
  return (v && *v) ? std::string(v) : def;
}

std::string base64(std::string_view in) {
  static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  size_t i = 0;
  while (i + 2 < in.size()) {
    const uint32_t v = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8) |
                       static_cast<unsigned char>(in[i + 2]);
    out.push_back(tbl[(v >> 18) & 63]);
    out.push_back(tbl[(v >> 12) & 63]);
    out.push_back(tbl[(v >> 6) & 63]);
    out.push_back(tbl[v & 63]);
    i += 3;
  }
  if (i + 1 == in.size()) {
    const uint32_t v = static_cast<unsigned char>(in[i]) << 16;
    out.push_back(tbl[(v >> 18) & 63]);
    out.push_back(tbl[(v >> 12) & 63]);
    out += "==";
  } else if (i + 2 == in.size()) {
    const uint32_t v = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8);
    out.push_back(tbl[(v >> 18) & 63]);
    out.push_back(tbl[(v >> 12) & 63]);
    out.push_back(tbl[(v >> 6) & 63]);
    out.push_back('=');
  }
  return out;
}

ImageRef parse_image_ref(std::string_view ref) {
  ImageRef r;
  const size_t at = ref.find('@');
  if (at != std::string_view::npos) {
    r.digest = std::string(ref.substr(at + 1));
    ref = ref.substr(0, at);
  }
  const size_t slash = ref.rfind('/');
  const size_t colon = ref.find(':', slash == std::string_view::npos ? 0 : slash + 1);
  if (colon != std::string_view::npos) {
    r.tag = std::string(ref.substr(colon + 1));
    r.name = std::string(ref.substr(0, colon));
  } else {
    r.name = std::string(ref);
  }
  return r;
}

}  // namespace podlogs::util
