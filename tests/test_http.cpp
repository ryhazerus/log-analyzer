#include "fake_server.hpp"
#include "harness.hpp"
#include "http.hpp"

using namespace podlogs::http;

TEST_CASE(endpoint_parsing) {
  auto u = parse_endpoint("unix:///run/podman/podman.sock");
  CHECK(u && u->is_unix() && u->unix_path == "/run/podman/podman.sock");
  auto p = parse_endpoint("/run/user/1000/podman/podman.sock");
  CHECK(p && p->is_unix());
  auto h = parse_endpoint("http://loki:3100");
  CHECK(h && !h->is_unix() && h->host == "loki" && h->port == 3100 && h->base_path.empty());
  auto b = parse_endpoint("http://10.0.0.5/loki/");
  CHECK(b && b->host == "10.0.0.5" && b->port == 80 && b->base_path == "/loki");
  auto v6 = parse_endpoint("http://[::1]:3100");
  CHECK(v6 && v6->host == "::1" && v6->port == 3100);
  CHECK(!parse_endpoint("https://loki:3100"));
  CHECK(!parse_endpoint("loki:3100"));
}

TEST_CASE(chunked_decoder_handles_split_input) {
  BodyDecoder d(BodyDecoder::Framing::Chunked);
  std::string out;
  CHECK(!d.feed("5;ext=1\r\nhel", out));
  CHECK(!d.feed("lo\r\n", out));
  CHECK(!d.feed("6\r\n wor", out));
  CHECK(!d.feed("ld\r", out));
  CHECK(!d.feed("\n0\r\nX-Trailer: yes\r\n", out));
  CHECK(d.feed("\r\n", out));
  CHECK_EQ(out, std::string("hello world"));
}

TEST_CASE(content_length_decoder) {
  BodyDecoder d(BodyDecoder::Framing::ContentLength, 5);
  std::string out;
  CHECK(!d.feed("ab", out));
  CHECK(d.feed("cde-ignored", out));
  CHECK_EQ(out, std::string("abcde"));
  BodyDecoder empty(BodyDecoder::Framing::ContentLength, 0);
  CHECK(empty.done());
}

TEST_CASE(response_head_parsing) {
  Response r;
  const std::string raw = "HTTP/1.1 204 No Content\r\nContent-Type: text/plain\r\nX-Foo:  bar \r\n\r\nrest";
  const size_t consumed = parse_response_head(raw, r);
  CHECK_EQ(consumed, raw.size() - 4);
  CHECK_EQ(r.status, 204);
  CHECK_EQ(r.reason, std::string("No Content"));
  CHECK_EQ(*r.header("x-foo"), std::string("bar"));
  CHECK_EQ(*r.header("Content-Type"), std::string("text/plain"));
  Response partial;
  CHECK_EQ(parse_response_head("HTTP/1.1 200 OK\r\nA: b\r\n", partial), size_t{0});
  CHECK_THROWS(parse_response_head("<html>\r\n\r\n", partial));
}

TEST_CASE(request_roundtrip_over_tcp) {
  auto server = testing::FakeServer::listen_tcp();
  server->set_handler([](const testing::FakeRequest& req, testing::FakeConn& conn) {
    if (req.method == "POST" && req.path == "/echo") conn.respond(200, req.body);
    else conn.respond(404, "nope", "text/plain");
  });
  server->start();
  Endpoint ep = *parse_endpoint(server->url());
  Request req;
  req.method = "POST";
  req.path = "/echo";
  req.body = "payload";
  const Response resp = request(ep, req, 2000);
  CHECK_EQ(resp.status, 200);
  CHECK_EQ(resp.body, std::string("payload"));
  Request bad;
  bad.path = "/missing";
  CHECK_EQ(request(ep, bad, 2000).status, 404);
}

TEST_CASE(streaming_over_unix_socket_with_cancel) {
  const std::string path = "/tmp/podlogs-test-" + std::to_string(getpid()) + ".sock";
  auto server = testing::FakeServer::listen_unix(path);
  server->set_handler([](const testing::FakeRequest&, testing::FakeConn& conn) {
    conn.begin_chunked();
    conn.chunk("first\n");
    conn.chunk("second\n");
    conn.hold_open();
  });
  server->start();
  Endpoint ep = *parse_endpoint("unix://" + path);
  Request req;
  req.path = "/stream";
  auto stream = open_stream(ep, req, 2000);
  CHECK_EQ(stream->status(), 200);
  std::string out;
  while (out.find("second\n") == std::string::npos) {
    const int n = stream->read(out, 1000);
    CHECK(n != 0);
  }
  CHECK_EQ(out, std::string("first\nsecond\n"));
  CHECK_EQ(stream->read(out, 100), -1);  // nothing more yet: timeout
  std::thread canceller([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stream->cancel();
  });
  CHECK_EQ(stream->read(out, 5000), 0);  // cancel produces EOF
  canceller.join();
}

TEST_CASE(connect_failure_is_an_error) {
  Endpoint ep = *parse_endpoint("unix:///nonexistent/podman.sock");
  Request req;
  CHECK_THROWS(request(ep, req, 500));
}
