#include "harness.hpp"
#include "util.hpp"

using namespace podlogs::util;

TEST_CASE(rfc3339_parses_utc_with_nanos) {
  auto ns = parse_rfc3339("2024-05-01T12:34:56.123456789Z");
  CHECK(ns.has_value());
  CHECK_EQ(*ns, 1714566896LL * kNanosPerSec + 123456789);
}

TEST_CASE(rfc3339_parses_offsets_and_short_fractions) {
  auto a = parse_rfc3339("2024-05-01T14:34:56.5+02:00");
  auto b = parse_rfc3339("2024-05-01T12:34:56Z");
  CHECK(a && b);
  CHECK_EQ(*a - *b, 500 * kNanosPerMs);
  auto c = parse_rfc3339("2024-05-01T10:04:56-0230");
  CHECK(c && *c == *b);
}

TEST_CASE(rfc3339_rejects_garbage) {
  CHECK(!parse_rfc3339("2024-05-01"));
  CHECK(!parse_rfc3339("hello world 2024"));
  CHECK(!parse_rfc3339("2024-05-01T12:34:56"));
  CHECK(!parse_rfc3339("2024-13-01T12:34:56Z"));
  CHECK(!parse_rfc3339("2024-05-01T12:34:56Zextra"));
}

TEST_CASE(rfc3339_roundtrip) {
  const Nanos ts = 1714566896LL * kNanosPerSec + 7;
  CHECK_EQ(format_rfc3339(ts), std::string("2024-05-01T12:34:56.000000007Z"));
  CHECK_EQ(*parse_rfc3339(format_rfc3339(ts)), ts);
}

TEST_CASE(duration_parsing) {
  CHECK_EQ(*parse_duration_ms("500ms"), 500LL);
  CHECK_EQ(*parse_duration_ms("30s"), 30000LL);
  CHECK_EQ(*parse_duration_ms("5m"), 300000LL);
  CHECK_EQ(*parse_duration_ms("1.5h"), 5400000LL);
  CHECK_EQ(*parse_duration_ms("2d"), 172800000LL);
  CHECK_EQ(*parse_duration_ms("45"), 45000LL);
  CHECK(!parse_duration_ms("soon"));
  CHECK(!parse_duration_ms(""));
}

TEST_CASE(image_ref_parsing) {
  auto a = parse_image_ref("registry.example.com:5000/team/orders:1.4.2");
  CHECK_EQ(a.name, std::string("registry.example.com:5000/team/orders"));
  CHECK_EQ(a.tag, std::string("1.4.2"));
  auto b = parse_image_ref("docker.io/library/nginx@sha256:abcdef");
  CHECK_EQ(b.name, std::string("docker.io/library/nginx"));
  CHECK_EQ(b.tag, std::string(""));
  CHECK_EQ(b.digest, std::string("sha256:abcdef"));
  auto c = parse_image_ref("nginx");
  CHECK_EQ(c.name, std::string("nginx"));
  CHECK_EQ(c.tag, std::string(""));
}

TEST_CASE(short_id_and_label_names) {
  CHECK_EQ(short_id("sha256:0123456789abcdef0123"), std::string("0123456789ab"));
  CHECK_EQ(short_id("abc"), std::string("abc"));
  CHECK_EQ(sanitize_label_name("io.podman.compose.project"), std::string("io_podman_compose_project"));
  CHECK_EQ(sanitize_label_name("9lives"), std::string("_9lives"));
  CHECK_EQ(sanitize_label_name(""), std::string("_"));
}

TEST_CASE(url_encode_and_base64) {
  CHECK_EQ(url_encode("2024-05-01T12:34:56.000000007Z"), std::string("2024-05-01T12%3A34%3A56.000000007Z"));
  CHECK_EQ(url_encode(R"({"type":["container"]})"), std::string("%7B%22type%22%3A%5B%22container%22%5D%7D"));
  CHECK_EQ(base64("user:pass"), std::string("dXNlcjpwYXNz"));
  CHECK_EQ(base64("a"), std::string("YQ=="));
  CHECK_EQ(base64("ab"), std::string("YWI="));
}
