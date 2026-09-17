// End-to-end: a fake podman API on a unix socket and a fake Loki on TCP, with
// the real TailManager / parser / LokiClient / Checkpoint in between.
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <thread>

#include "checkpoint.hpp"
#include "config.hpp"
#include "fake_server.hpp"
#include "harness.hpp"
#include "loki.hpp"
#include "nlohmann/json.hpp"
#include "podman.hpp"
#include "tailer.hpp"

using namespace podlogs;
using nlohmann::json;

namespace {

const char* kOrdersId = "1111111111111111111111111111111111111111111111111111111111111111";
const char* kWebId = "2222222222222222222222222222222222222222222222222222222222222222";
const char* kInfraId = "3333333333333333333333333333333333333333333333333333333333333333";
const char* kAgentId = "4444444444444444444444444444444444444444444444444444444444444444";

const char* T1 = "2024-05-01T12:00:00.100000000Z";
const char* T2 = "2024-05-01T12:00:00.200000000Z";
const char* T3 = "2024-05-01T12:00:00.300000000Z";
const char* T4 = "2024-05-01T12:00:00.300000000Z";  // same stamp as T3: one write, two lines
const char* T5 = "2024-05-01T12:00:00.400000000Z";
const char* T6 = "2024-05-01T12:00:01.000000000Z";

std::string containers_json() {
  json list = json::array();
  list.push_back({{"Id", kOrdersId},
                  {"Names", json::array({"orders"})},
                  {"Image", "registry.example.com/team/orders:1.2.3"},
                  {"ImageID", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
                  {"State", "running"},
                  {"PodName", ""},
                  {"IsInfra", false},
                  {"StartedAt", 1714566000},
                  {"Labels", {{"io.podman.compose.project", "shop"}}}});
  list.push_back({{"Id", kWebId},
                  {"Names", json::array({"web"})},
                  {"Image", "docker.io/library/nginx:1.25"},
                  {"ImageID", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"},
                  {"State", "running"},
                  {"PodName", "frontend"},
                  {"IsInfra", false},
                  {"StartedAt", 1714566000},
                  {"Labels", json::object()}});
  list.push_back({{"Id", kInfraId},
                  {"Names", json::array({"frontend-infra"})},
                  {"Image", "localhost/podman-pause:5"},
                  {"ImageID", "cccc"},
                  {"State", "running"},
                  {"PodName", "frontend"},
                  {"IsInfra", true},
                  {"StartedAt", 1714566000},
                  {"Labels", json::object()}});
  list.push_back({{"Id", kAgentId},
                  {"Names", json::array({"podlogs-agent"})},
                  {"Image", "ghcr.io/x/podlogs:1"},
                  {"ImageID", "dddd"},
                  {"State", "running"},
                  {"PodName", ""},
                  {"IsInfra", false},
                  {"StartedAt", 1714566000},
                  {"Labels", json::object()}});
  return list.dump();
}

struct Received {
  std::map<std::string, std::string> labels;
  std::string line;
  std::map<std::string, std::string> meta;
};

struct Harness {
  std::string dir;
  std::unique_ptr<testing::FakeServer> podman;
  std::unique_ptr<testing::FakeServer> loki;
  std::mutex mu;
  std::vector<Received> received;

  Harness() {
    dir = (std::filesystem::temp_directory_path() / ("podlogs-e2e-" + std::to_string(getpid()))).string();
    std::filesystem::create_directories(dir);
    podman = testing::FakeServer::listen_unix(dir + "/p.sock");
    podman->set_handler([this](const testing::FakeRequest& req, testing::FakeConn& conn) { handle_podman(req, conn); });
    podman->start();
    loki = testing::FakeServer::listen_tcp();
    loki->set_handler([this](const testing::FakeRequest& req, testing::FakeConn& conn) {
      if (req.path == "/loki/api/v1/push") {
        const json doc = json::parse(req.body);
        std::lock_guard<std::mutex> lk(mu);
        for (const auto& s : doc["streams"]) {
          for (const auto& v : s["values"]) {
            Received r;
            r.labels = s["stream"].get<std::map<std::string, std::string>>();
            r.line = v[1].get<std::string>();
            if (v.size() > 2) r.meta = v[2].get<std::map<std::string, std::string>>();
            received.push_back(std::move(r));
          }
        }
        conn.respond(204, "");
      } else if (req.path == "/ready") {
        conn.respond(200, "ready\n", "text/plain");
      } else {
        conn.respond(404, "");
      }
    });
    loki->start();
  }

  ~Harness() {
    podman->stop();
    loki->stop();
    std::filesystem::remove_all(dir);
  }

  void handle_podman(const testing::FakeRequest& req, testing::FakeConn& conn) {
    const std::string& p = req.path;
    if (p.rfind("/v4.0.0/libpod/version", 0) == 0) {
      conn.respond(200, R"({"Version":"4.9.4-fake"})");
    } else if (p.rfind("/v4.0.0/libpod/containers/json", 0) == 0) {
      conn.respond(200, containers_json());
    } else if (p.rfind("/v4.0.0/libpod/events", 0) == 0) {
      conn.begin_chunked();
      conn.hold_open();
      conn.end_chunked();
    } else if (p.rfind(std::string("/v4.0.0/libpod/containers/") + kOrdersId + "/logs", 0) == 0) {
      CHECK(p.find("follow=true") != std::string::npos);
      CHECK(p.find("timestamps=true") != std::string::npos);
      conn.begin_chunked();
      conn.chunk(testing::log_frame(1, T1, R"({"level":"error","msg":"payment failed","logger":"orders.Pay"})"));
      conn.chunk(testing::log_frame(1, T2, "2024-05-01 12:00:00.200 INFO  Started OrdersApplication"));
      // A stack trace split into three frames, the way conmon writes it.
      std::string trace = testing::log_frame(2, T3, "Exception in thread \"main\"");
      trace += testing::log_frame(2, T4, "java.lang.RuntimeException: boom");
      trace += testing::log_frame(2, T5, "\tat a.b.C(C.java:1)");
      conn.chunk(trace);
      conn.hold_open();
      conn.end_chunked();
    } else if (p.rfind(std::string("/v4.0.0/libpod/containers/") + kWebId + "/logs", 0) == 0) {
      conn.begin_chunked();
      conn.chunk(testing::log_frame(1, T6, "10.0.0.1 - - [01/May/2024:12:00:01 +0000] \"GET / HTTP/1.1\" 200 612"));
      conn.hold_open();
      conn.end_chunked();
    } else {
      conn.respond(404, "unexpected path " + p, "text/plain");
    }
  }

  size_t count() {
    std::lock_guard<std::mutex> lk(mu);
    return received.size();
  }

  const Received* find(const std::string& needle) {
    for (const auto& r : received) {
      if (r.line.find(needle) != std::string::npos) return &r;
    }
    return nullptr;
  }
};

Config make_config(const Harness& h) {
  Config cfg;
  cfg.podman.socket = h.podman->path();
  cfg.podman.initial_lookback = "all";
  cfg.podman.resync_interval_ms = 300;
  cfg.loki.url = h.loki->url();
  cfg.loki.batch_wait_ms = 100;
  cfg.checkpoint.path = h.dir + "/checkpoint.json";
  cfg.metrics_listen = "";
  cfg.hostname = "test-host";
  validate_config(cfg);
  return cfg;
}

// Runs the manager until `until` returns true (or timeout), then shuts down cleanly.
void run_pipeline(Harness& h, const Config& cfg, Checkpoint& checkpoint, const std::function<bool()>& until) {
  PodmanClient podman(*http::parse_endpoint(cfg.podman.socket), cfg.podman.api_prefix, cfg.podman.timeout_ms);
  Metrics metrics;
  BoundedQueue<LogEntry> queue(cfg.queue_capacity);
  LineParser parser(cfg.parse);
  TailContext ctx{cfg, podman, parser, checkpoint, queue, metrics, cfg.hostname};
  LokiClient loki(cfg.loki, queue, metrics,
                  [&](const std::string& id, const std::string& name, util::Nanos ts) { checkpoint.advance(id, ts, name); });
  loki.start();
  std::atomic<bool> stop{false};
  TailManager manager(ctx);
  std::thread runner([&] { manager.run(stop); });
  for (int i = 0; i < 400 && !until(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(25));
  stop.store(true);
  runner.join();
  queue.close();
  loki.stop(1000);
  (void)h;
}

}  // namespace

TEST_CASE(e2e_tails_selected_containers_and_ships_to_loki) {
  Harness h;
  const Config cfg = make_config(h);
  Checkpoint checkpoint(cfg.checkpoint.path);

  run_pipeline(h, cfg, checkpoint, [&] { return h.count() >= 4; });

  std::lock_guard<std::mutex> lk(h.mu);
  CHECK_EQ(h.received.size(), size_t{4});

  const Received* json_line = h.find("payment failed");
  CHECK(json_line != nullptr);
  CHECK_EQ(json_line->labels.at("job"), std::string("podlogs"));
  CHECK_EQ(json_line->labels.at("host"), std::string("test-host"));
  CHECK_EQ(json_line->labels.at("container_name"), std::string("orders"));
  CHECK_EQ(json_line->labels.at("image_name"), std::string("registry.example.com/team/orders"));
  CHECK_EQ(json_line->labels.at("image_tag"), std::string("1.2.3"));
  CHECK_EQ(json_line->labels.at("image_id"), std::string("aaaaaaaaaaaa"));
  CHECK_EQ(json_line->labels.at("stream"), std::string("stdout"));
  CHECK_EQ(json_line->labels.at("compose_project"), std::string("shop"));
  CHECK(!json_line->labels.count("pod"));
  CHECK_EQ(json_line->meta.at("detected_level"), std::string("error"));
  CHECK_EQ(json_line->meta.at("logger"), std::string("orders.Pay"));
  CHECK_EQ(json_line->meta.at("container_id"), std::string("111111111111"));

  const Received* text_line = h.find("Started OrdersApplication");
  CHECK(text_line != nullptr);
  CHECK_EQ(text_line->meta.at("detected_level"), std::string("info"));

  const Received* trace = h.find("Exception in thread");
  CHECK(trace != nullptr);
  CHECK_EQ(trace->line, std::string("Exception in thread \"main\"\njava.lang.RuntimeException: boom\n\tat a.b.C(C.java:1)"));
  CHECK_EQ(trace->labels.at("stream"), std::string("stderr"));

  const Received* web = h.find("GET / HTTP/1.1");
  CHECK(web != nullptr);
  CHECK_EQ(web->labels.at("pod"), std::string("frontend"));
  CHECK_EQ(web->labels.at("image_name"), std::string("docker.io/library/nginx"));
  CHECK(!web->meta.count("detected_level"));

  // Infra and excluded containers were never tailed.
  for (const auto& r : h.podman->requests()) {
    CHECK(r.find(kInfraId) == std::string::npos);
    CHECK(r.find(kAgentId) == std::string::npos);
  }

  // Loki acknowledged everything, so the checkpoint is at the newest stamp per container.
  CHECK_EQ(*checkpoint.get(kOrdersId), *util::parse_rfc3339(T5));
  CHECK_EQ(*checkpoint.get(kWebId), *util::parse_rfc3339(T6));
  CHECK(checkpoint.save());
}

TEST_CASE(e2e_resumes_from_checkpoint) {
  Harness h;
  const Config cfg = make_config(h);
  Checkpoint checkpoint(cfg.checkpoint.path);
  const util::Nanos resume_at = *util::parse_rfc3339(T5);
  checkpoint.advance(kOrdersId, resume_at, "orders");

  run_pipeline(h, cfg, checkpoint, [&] { return h.count() >= 2; });

  bool saw_since = false;
  for (const auto& r : h.podman->requests()) {
    if (r.find(kOrdersId) != std::string::npos && r.find("/logs") != std::string::npos) {
      CHECK(r.find("since=" + util::url_encode(util::format_rfc3339(resume_at))) != std::string::npos);
      saw_since = true;
    }
    if (r.find(kWebId) != std::string::npos && r.find("/logs") != std::string::npos) {
      CHECK(r.find("since=") == std::string::npos);  // no checkpoint + lookback "all"
    }
  }
  CHECK(saw_since);

  // The fake server replays everything regardless of `since`; the tailer must
  // drop entries older than the checkpoint client-side (T1, T2) and keep the
  // stack trace whose first line is stamped T3 < T5... which is dropped too,
  // leaving only the "\tat" line (T5) and the web line.
  std::lock_guard<std::mutex> lk(h.mu);
  CHECK(h.find("payment failed") == nullptr);
  CHECK(h.find("Started OrdersApplication") == nullptr);
  CHECK(h.find("GET / HTTP/1.1") != nullptr);
}
