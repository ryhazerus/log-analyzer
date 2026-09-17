#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace podlogs::util {

// Nanoseconds since the Unix epoch. Loki wants nanosecond timestamps and podman
// emits RFC3339Nano, so everything internal is kept in this unit.
using Nanos = int64_t;
constexpr Nanos kNanosPerSec = 1'000'000'000LL;
constexpr Nanos kNanosPerMs = 1'000'000LL;

Nanos now_nanos();

// Parses RFC3339 / ISO-8601 timestamps such as 2024-05-01T12:34:56.123456789Z
// or 2024-05-01T12:34:56+02:00. Returns nullopt on malformed input.
std::optional<Nanos> parse_rfc3339(std::string_view s);

// Formats as RFC3339Nano in UTC, e.g. 2024-05-01T12:34:56.123456789Z.
std::string format_rfc3339(Nanos ns);

// Parses "500ms", "30s", "5m", "1h", "2d" or a bare number of seconds.
std::optional<int64_t> parse_duration_ms(std::string_view s);

std::string hostname();
std::string to_lower(std::string_view s);
std::string to_upper(std::string_view s);
bool starts_with(std::string_view s, std::string_view prefix);
std::string_view trim(std::string_view s);
std::vector<std::string> split(std::string_view s, char sep);

// First n characters of a container/image id (podman style short id).
std::string short_id(std::string_view id, size_t n = 12);

// Makes a string a valid Prometheus/Loki label name: [A-Za-z_][A-Za-z0-9_]*.
std::string sanitize_label_name(std::string_view s);

std::string url_encode(std::string_view s);
std::string getenv_or(const char* name, const std::string& def);
std::string base64(std::string_view in);

struct ImageRef {
  std::string name;    // registry/repo without tag or digest
  std::string tag;     // may be empty
  std::string digest;  // may be empty
};
ImageRef parse_image_ref(std::string_view ref);

}  // namespace podlogs::util
