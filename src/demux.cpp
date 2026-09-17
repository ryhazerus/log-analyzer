#include "demux.hpp"

namespace podlogs {

bool Demuxer::looks_like_header(std::string_view b) {
  if (b.size() < 8) return false;
  const unsigned char type = static_cast<unsigned char>(b[0]);
  return type <= 2 && b[1] == 0 && b[2] == 0 && b[3] == 0;
}

void Demuxer::feed(std::string_view data, std::vector<RawLine>& out) {
  buf_.append(data);

  if (mode_ == Mode::Detect) {
    if (buf_.size() >= 8) {
      mode_ = looks_like_header(buf_) ? Mode::Framed : Mode::Raw;
    } else if (buf_.find('\n') != std::string::npos) {
      mode_ = Mode::Raw;
    } else {
      return;
    }
  }

  if (mode_ == Mode::Framed) {
    size_t pos = 0;
    while (buf_.size() - pos >= 8) {
      const std::string_view hdr(buf_.data() + pos, 8);
      if (!looks_like_header(hdr)) {
        // Lost framing; treat the rest as raw text rather than dropping it.
        mode_ = Mode::Raw;
        break;
      }
      const int type = static_cast<unsigned char>(hdr[0]);
      const size_t len = (static_cast<size_t>(static_cast<unsigned char>(hdr[4])) << 24) |
                         (static_cast<size_t>(static_cast<unsigned char>(hdr[5])) << 16) |
                         (static_cast<size_t>(static_cast<unsigned char>(hdr[6])) << 8) |
                         static_cast<size_t>(static_cast<unsigned char>(hdr[7]));
      if (buf_.size() - pos - 8 < len) break;
      emit_payload(type, std::string_view(buf_.data() + pos + 8, len), out);
      pos += 8 + len;
    }
    buf_.erase(0, pos);
    if (mode_ == Mode::Framed) return;
  }

  // Raw mode: newline separated.
  size_t start = 0;
  while (true) {
    const size_t nl = buf_.find('\n', start);
    if (nl == std::string::npos) break;
    emit_line("stdout", std::string_view(buf_.data() + start, nl - start), 0, out);
    start = nl + 1;
  }
  buf_.erase(0, start);
}

void Demuxer::finish(std::vector<RawLine>& out) {
  if (mode_ != Mode::Framed && !buf_.empty()) emit_line("stdout", buf_, 0, out);
  buf_.clear();
}

void Demuxer::emit_payload(int type, std::string_view payload, std::vector<RawLine>& out) {
  const char* stream = type == 2 ? "stderr" : "stdout";
  // Normally one frame is one line, but be tolerant of embedded newlines: the
  // first line carries the timestamp, the rest inherit it.
  util::Nanos inherited = 0;
  size_t start = 0;
  while (start < payload.size()) {
    size_t nl = payload.find('\n', start);
    if (nl == std::string_view::npos) nl = payload.size();
    const size_t before = out.size();
    emit_line(stream, payload.substr(start, nl - start), inherited, out);
    if (out.size() > before) inherited = out.back().ts;
    start = nl + 1;
  }
}

void Demuxer::emit_line(const char* stream, std::string_view line, util::Nanos inherited_ts,
                        std::vector<RawLine>& out) {
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
  if (line.empty()) return;

  util::Nanos ts = 0;
  std::string_view text = line;
  const size_t sp = line.find(' ');
  if (sp != std::string_view::npos && sp >= 20) {
    if (auto parsed = util::parse_rfc3339(line.substr(0, sp))) {
      ts = *parsed;
      text = line.substr(sp + 1);
    }
  }
  if (ts == 0) ts = inherited_ts != 0 ? inherited_ts : util::now_nanos();
  while (!text.empty() && (text.back() == '\r' || text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  if (text.empty()) return;

  out.push_back(RawLine{stream, ts, std::string(text)});
}

}  // namespace podlogs
