#include <atomic>

#include "fake_server.hpp"
#include "harness.hpp"
#include "loki.hpp"
#include "nlohmann/json.hpp"

using namespace podlogs;
using nlohmann::json;

namespace {

std::shared_ptr<StreamIdentity> ident(const std::string& name, const std::string& stream) {
  auto id = std::make_shared<StreamIdentity>();
  id->container_id = name + "-id";
  id->container_name = name;
  id->labels = {{"job", "podlogs"}, {"container_name", name}, {"stream", stream}};
  id->metadata = {{"container_id", name.substr(0, 3)}};
  id->key = name + "/" + stream;
  return id;
}

LogEntry entry(std::shared_ptr<StreamIdentity> id, util::Nanos ts, const std::string& line, const std::string& level = "") {
  LogEntry e;
  e.stream = std::move(id);
  e.ts = ts;
  e.line = line;
  e.level = level;
  return e;
}

}  // namespace

TEST_CASE(push_payload_groups_sorts_and_attaches_metadata) {
  auto a = ident("orders", "stdout");
  auto b = ident("orders", "stderr");
  std::vector<LogEntry> entries = {
      entry(a, 20, "second", "info"),
      entry(b, 5, "err line", "error"),
      entry(a, 10, "first"),
  };
  entries[0].logger = "orders.Svc";
  const json doc = json::parse(build_push_payload(entries));
  CHECK_EQ(doc["streams"].size(), size_t{2});
  const json& s0 = doc["streams"][0];
  CHECK_EQ(s0["stream"]["container_name"], std::string("orders"));
  CHECK_EQ(s0["stream"]["stream"], std::string("stdout"));
  CHECK_EQ(s0["values"].size(), size_t{2});
  CHECK_EQ(s0["values"][0][0], std::string("10"));  // sorted by ts
  CHECK_EQ(s0["values"][0][1], std::string("first"));
  CHECK_EQ(s0["values"][0][2]["container_id"], std::string("ord"));
  CHECK(!s0["values"][0][2].contains("detected_level"));
  CHECK_EQ(s0["values"][1][2]["detected_level"], std::string("info"));
  CHECK_EQ(s0["values"][1][2]["logger"], std::string("orders.Svc"));
  CHECK_EQ(doc["streams"][1]["values"][0][2]["detected_level"], std::string("error"));
}

TEST_CASE(push_payload_survives_invalid_utf8) {
  auto a = ident("bin", "stdout");
  std::vector<LogEntry> entries = {entry(a, 1, std::string("bad \xff\xfe bytes"))};
  const std::string body = build_push_payload(entries);
  CHECK(json::accept(body));
}

TEST_CASE(client_ships_batches_and_acks_per_container) {
  auto server = testing::FakeServer::listen_tcp();
  std::mutex mu;
  std::vector<json> received;
  std::atomic<int> failures_left{2};
  server->set_handler([&](const testing::FakeRequest& req, testing::FakeConn& conn) {
    if (req.path == "/loki/api/v1/push") {
      if (failures_left.fetch_sub(1) > 0) {
        conn.respond(503, "not ready yet", "text/plain");  // transient: must be retried
        return;
      }
      std::lock_guard<std::mutex> lk(mu);
      received.push_back(json::parse(req.body));
      CHECK_EQ(req.headers.at("x-scope-orgid"), std::string("team-a"));
      CHECK_EQ(req.headers.at("authorization"), std::string("Basic dXNlcjpwYXNz"));
      conn.respond(204, "");
    } else {
      conn.respond(404, "");
    }
  });
  server->start();

  LokiConfig cfg;
  cfg.url = server->url();
  cfg.batch_wait_ms = 50;
  cfg.tenant = "team-a";
  cfg.basic_auth_user = "user";
  cfg.basic_auth_pass = "pass";
  cfg.backoff_max_ms = 100;
  BoundedQueue<LogEntry> queue(1000);
  Metrics metrics;
  std::map<std::string, util::Nanos> acks;
  LokiClient client(cfg, queue, metrics, [&](const std::string& id, const std::string& name, util::Nanos ts) {
    std::lock_guard<std::mutex> lk(mu);
    acks[id + "|" + name] = ts;
  });
  client.start();

  auto a = ident("orders", "stdout");
  auto b = ident("web", "stdout");
  queue.push(entry(a, 100, "x"), std::chrono::seconds(1));
  queue.push(entry(a, 300, "y"), std::chrono::seconds(1));
  queue.push(entry(b, 200, "z"), std::chrono::seconds(1));

  for (int i = 0; i < 200; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    std::lock_guard<std::mutex> lk(mu);
    if (!received.empty()) break;
  }
  queue.close();
  client.stop(1000);

  std::lock_guard<std::mutex> lk(mu);
  CHECK_EQ(received.size(), size_t{1});
  CHECK_EQ(received[0]["streams"].size(), size_t{2});
  CHECK_EQ(acks["orders-id|orders"], 300LL);
  CHECK_EQ(acks["web-id|web"], 200LL);
  CHECK_EQ(metrics.push_retries.load(), uint64_t{2});
  CHECK_EQ(metrics.batches_sent.load(), uint64_t{1});
  CHECK_EQ(metrics.entries_shipped.load(), uint64_t{3});
}

TEST_CASE(client_drops_batches_rejected_with_4xx) {
  auto server = testing::FakeServer::listen_tcp();
  server->set_handler([&](const testing::FakeRequest&, testing::FakeConn& conn) {
    conn.respond(400, "entry too far behind", "text/plain");
  });
  server->start();
  LokiConfig cfg;
  cfg.url = server->url();
  cfg.batch_wait_ms = 20;
  BoundedQueue<LogEntry> queue(100);
  Metrics metrics;
  int acks = 0;
  LokiClient client(cfg, queue, metrics, [&](const std::string&, const std::string&, util::Nanos) { ++acks; });
  client.start();
  queue.push(entry(ident("x", "stdout"), 1, "old"), std::chrono::seconds(1));
  for (int i = 0; i < 100 && metrics.batches_dropped.load() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  queue.close();
  client.stop(1000);
  CHECK_EQ(metrics.batches_dropped.load(), uint64_t{1});
  CHECK_EQ(metrics.push_retries.load(), uint64_t{0});
  CHECK_EQ(acks, 0);
}
