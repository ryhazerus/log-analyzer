#include "demux.hpp"
#include "fake_server.hpp"
#include "harness.hpp"

using namespace podlogs;

TEST_CASE(demux_framed_lines_with_streams_and_timestamps) {
  Demuxer d;
  std::vector<RawLine> out;
  std::string data = testing::log_frame(1, "2024-05-01T12:00:00.000000001Z", "hello");
  data += testing::log_frame(2, "2024-05-01T12:00:00.000000002Z", "oops\r");
  d.feed(data, out);
  CHECK_EQ(out.size(), size_t{2});
  CHECK_EQ(out[0].stream, std::string("stdout"));
  CHECK_EQ(out[0].text, std::string("hello"));
  CHECK_EQ(out[0].ts, *util::parse_rfc3339("2024-05-01T12:00:00.000000001Z"));
  CHECK_EQ(out[1].stream, std::string("stderr"));
  CHECK_EQ(out[1].text, std::string("oops"));
  CHECK(d.framed());
}

TEST_CASE(demux_handles_frames_split_across_reads) {
  Demuxer d;
  std::vector<RawLine> out;
  const std::string frame = testing::log_frame(1, "2024-05-01T12:00:00Z", "a fairly long log line for splitting");
  for (size_t i = 0; i < frame.size(); i += 3) d.feed(frame.substr(i, 3), out);
  CHECK_EQ(out.size(), size_t{1});
  CHECK_EQ(out[0].text, std::string("a fairly long log line for splitting"));
}

TEST_CASE(demux_falls_back_to_raw_text) {
  Demuxer d;
  std::vector<RawLine> out;
  d.feed("2024-05-01T12:00:00Z raw line one\n2024-05-01T12:00:01Z raw line two\npartial", out);
  CHECK_EQ(out.size(), size_t{2});
  CHECK_EQ(out[1].text, std::string("raw line two"));
  CHECK(!d.framed());
  d.finish(out);
  CHECK_EQ(out.size(), size_t{3});
  CHECK_EQ(out[2].text, std::string("partial"));
  CHECK(out[2].ts > 0);  // no timestamp prefix: now
}

TEST_CASE(demux_skips_blank_lines_and_keeps_untimestamped_text) {
  Demuxer d;
  std::vector<RawLine> out;
  std::string data = testing::log_frame(1, "2024-05-01T12:00:00Z", "");
  data += testing::log_frame(1, "2024-05-01T12:00:00Z", "   ");
  data += testing::log_frame(1, "2024-05-01T12:00:00Z", "multi\nline payload");
  d.feed(data, out);
  CHECK_EQ(out.size(), size_t{2});
  CHECK_EQ(out[0].text, std::string("multi"));
  CHECK_EQ(out[1].text, std::string("line payload"));
  CHECK_EQ(out[0].ts, out[1].ts);  // inherited
}
