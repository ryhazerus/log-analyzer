#include "harness.hpp"
#include "parse.hpp"

using namespace podlogs;

namespace {
RawLine line(const char* text, const char* stream = "stdout") { return RawLine{stream, 1, text}; }
}  // namespace

TEST_CASE(level_normalization) {
  CHECK_EQ(normalize_level("WARNING"), std::string("warn"));
  CHECK_EQ(normalize_level("Err"), std::string("error"));
  CHECK_EQ(normalize_level(" critical "), std::string("fatal"));
  CHECK_EQ(normalize_level("SEVERE"), std::string("error"));
  CHECK_EQ(normalize_level("finest"), std::string("trace"));
  CHECK_EQ(normalize_level("notice"), std::string("info"));
  CHECK_EQ(normalize_level("banana"), std::string(""));
  CHECK_EQ(level_from_number(50, "pino"), std::string("error"));
  CHECK_EQ(level_from_number(30, "pino"), std::string("info"));
  CHECK_EQ(level_from_number(30, "python"), std::string("warn"));
}

TEST_CASE(json_lines_extract_level_and_logger) {
  LineParser p{ParseConfig{}};
  auto a = p.parse(R"({"level":"ERROR","msg":"boom","logger":"orders.Service"})");
  CHECK(a.is_json);
  CHECK_EQ(a.level, std::string("error"));
  CHECK_EQ(a.logger, std::string("orders.Service"));
  auto b = p.parse(R"({"log":{"level":"warn","logger":"ecs"},"message":"x"})");
  CHECK_EQ(b.level, std::string("warn"));
  CHECK_EQ(b.logger, std::string("ecs"));
  auto c = p.parse(R"({"level":30,"time":1714566896000,"msg":"pino style"})");
  CHECK_EQ(c.level, std::string("info"));
  auto d = p.parse(R"({"severity":"CRITICAL","message":"gcp style"})");
  CHECK_EQ(d.level, std::string("fatal"));
  auto e = p.parse(R"({"msg":"no level here"})");
  CHECK(e.is_json && e.level.empty());
}

TEST_CASE(invalid_json_is_treated_as_text) {
  LineParser p{ParseConfig{}};
  auto a = p.parse(R"({"level":"error", broken)");
  CHECK(!a.is_json);
  CHECK_EQ(a.level, std::string("error"));  // text detection still finds the word
  auto b = p.parse("[1,2,3]");
  CHECK(!b.is_json);
}

TEST_CASE(text_level_detection) {
  LineParser p{ParseConfig{}};
  CHECK_EQ(p.parse("2024-05-01 12:00:00.123  WARN 1 --- [main] c.e.Foo : slow").level, std::string("warn"));
  CHECK_EQ(p.parse("[ERROR] something failed").level, std::string("error"));
  CHECK_EQ(p.parse("level=info msg=\"listening\"").level, std::string("info"));
  CHECK_EQ(p.parse("INFO: python style").level, std::string("info"));
  CHECK_EQ(p.parse("error_count=0 all good").level, std::string(""));  // token is error_count
  CHECK_EQ(p.parse("GET /healthz 200 0.3ms").level, std::string(""));
  CHECK_EQ(p.parse("Connection refused, will retry").level, std::string(""));
}

TEST_CASE(continuation_detection) {
  LineParser p{ParseConfig{}};
  CHECK(p.is_continuation("\tat com.example.Foo.bar(Foo.java:42)"));
  CHECK(p.is_continuation("at com.example.Foo.bar(Foo.java:42)"));
  CHECK(p.is_continuation("Caused by: java.io.IOException: disk"));
  CHECK(p.is_continuation("... 12 more"));
  CHECK(p.is_continuation("java.lang.IllegalStateException: boom"));
  CHECK(p.is_continuation("System.InvalidOperationException: nope"));
  CHECK(p.is_continuation("ValueError: bad value"));
  CHECK(p.is_continuation("requests.exceptions.HTTPError: 500"));
  CHECK(p.is_continuation("Traceback (most recent call last):"));
  CHECK(p.is_continuation("goroutine 1 [running]:"));
  CHECK(!p.is_continuation("Error: connection refused"));
  CHECK(!p.is_continuation("2024-05-01 12:00:00 INFO started"));
  CHECK(!p.is_continuation(R"({"level":"info"})"));
  CHECK(!p.is_continuation("Attempting reconnect"));
}

TEST_CASE(multiline_joins_stack_traces) {
  LineParser p{ParseConfig{}};
  MultilineAggregator agg(p);
  std::vector<RawLine> out;
  agg.push(line("2024-05-01 12:00:00 ERROR Request failed"), out);
  agg.push(line("java.lang.IllegalStateException: boom"), out);
  agg.push(line("\tat com.example.Foo.bar(Foo.java:42)"), out);
  agg.push(line("\tat com.example.Main.main(Main.java:7)"), out);
  CHECK_EQ(out.size(), size_t{0});
  agg.push(line("2024-05-01 12:00:01 INFO next request"), out);
  CHECK_EQ(out.size(), size_t{1});
  CHECK_EQ(out[0].text, std::string("2024-05-01 12:00:00 ERROR Request failed\njava.lang.IllegalStateException: boom\n"
                                    "\tat com.example.Foo.bar(Foo.java:42)\n\tat com.example.Main.main(Main.java:7)"));
  agg.flush(out);
  CHECK_EQ(out.size(), size_t{2});
  CHECK_EQ(out[1].text, std::string("2024-05-01 12:00:01 INFO next request"));
}

TEST_CASE(multiline_flushes_on_timeout_and_limits) {
  ParseConfig cfg;
  cfg.multiline.flush_timeout_ms = 10;
  cfg.multiline.max_lines = 2;
  LineParser p{cfg};
  MultilineAggregator agg(p);
  std::vector<RawLine> out;
  agg.push(line("first"), out);
  agg.push(line("  indented"), out);
  agg.push(line("  over the limit"), out);  // max_lines reached: starts a new entry
  CHECK_EQ(out.size(), size_t{1});
  CHECK_EQ(out[0].text, std::string("first\n  indented"));
  agg.flush_if_stale(util::now_nanos(), out);
  CHECK_EQ(out.size(), size_t{1});  // not stale yet
  agg.flush_if_stale(util::now_nanos() + 50 * util::kNanosPerMs, out);
  CHECK_EQ(out.size(), size_t{2});
  CHECK(!agg.has_pending());
}

TEST_CASE(multiline_disabled_passes_through) {
  ParseConfig cfg;
  cfg.multiline.enabled = false;
  LineParser p{cfg};
  MultilineAggregator agg(p);
  std::vector<RawLine> out;
  agg.push(line("a"), out);
  agg.push(line("  b"), out);
  CHECK_EQ(out.size(), size_t{2});
}

TEST_CASE(a_continuation_with_nothing_pending_stands_alone) {
  LineParser p{ParseConfig{}};
  MultilineAggregator agg(p);
  std::vector<RawLine> out;
  agg.push(line("\tat lonely.Frame(x.java:1)"), out);
  agg.flush(out);
  CHECK_EQ(out.size(), size_t{1});
}
