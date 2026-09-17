#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "util.hpp"

namespace podlogs {

struct RawLine {
  std::string stream;  // "stdout" or "stderr"
  util::Nanos ts = 0;  // stamp of the (first) line
  std::string text;
  util::Nanos ts_last = 0;  // stamp of the last joined line for multi-line entries; 0 = same as ts
};

// Turns the byte stream of a podman log follow into lines.
//
// Podman frames log output docker-style: an 8-byte header
// [stream_type, 0, 0, 0, len_be32] followed by `len` bytes. Because we always
// request timestamps, each line starts with an RFC3339Nano stamp and a space.
// If the first bytes do not look like a frame header the demuxer falls back to
// treating the input as raw newline-separated text (TTY containers on some
// podman versions).
class Demuxer {
 public:
  void feed(std::string_view data, std::vector<RawLine>& out);
  // Flushes any unterminated raw-mode line at end of stream.
  void finish(std::vector<RawLine>& out);
  bool framed() const { return mode_ == Mode::Framed; }

 private:
  enum class Mode { Detect, Framed, Raw };

  static bool looks_like_header(std::string_view b);
  void emit_payload(int type, std::string_view payload, std::vector<RawLine>& out);
  void emit_line(const char* stream, std::string_view line, util::Nanos inherited_ts, std::vector<RawLine>& out);

  Mode mode_ = Mode::Detect;
  std::string buf_;
};

}  // namespace podlogs
