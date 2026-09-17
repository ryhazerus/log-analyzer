#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "config.hpp"
#include "harness.hpp"
#include "podman.hpp"
#include "tailer.hpp"

using namespace podlogs;

namespace {
std::string write_temp(const std::string& content) {
  const std::string path =
      (std::filesystem::temp_directory_path() / ("podlogs-cfg-" + std::to_string(getpid()) + "-" + std::to_string(rand()) + ".json")).string();
  std::ofstream out(path);
  out << content;
  return path;
}
}  // namespace

TEST_CASE(config_defaults_and_file_with_comments) {
  const std::string path = write_temp(R"({
    // comments are fine
    "log_level": "debug",
    "podman": { "socket": "/tmp/x.sock", "resync_interval": "30s", "exclude": ["^podlogs", "^loki"] },
    "loki": { "url": "http://loki:3100", "batch_wait": 2000, "tenant": "t1" },
    "parse": { "multiline": { "extra_patterns": ["^--> "] } },
    "labels": { "static": { "job": "podlogs", "env": "prod" }, "from_container": ["image_name", "stream"] }
  })");
  Config cfg = load_config(path);
  std::filesystem::remove(path);
  CHECK_EQ(cfg.log_level, std::string("debug"));
  CHECK_EQ(cfg.podman.socket, std::string("/tmp/x.sock"));
  CHECK_EQ(cfg.podman.resync_interval_ms, 30000);
  CHECK_EQ(cfg.podman.exclude.size(), size_t{2});
  CHECK_EQ(cfg.loki.url, std::string("http://loki:3100"));
  CHECK_EQ(cfg.loki.batch_wait_ms, 2000);
  CHECK_EQ(cfg.loki.tenant, std::string("t1"));
  CHECK_EQ(cfg.parse.multiline.continuation_patterns.back(), std::string("^--> "));
  CHECK_EQ(cfg.labels.static_labels.at("env"), std::string("prod"));
  CHECK_EQ(cfg.labels.from_container.size(), size_t{2});
  CHECK(!dump_config(cfg).empty());
}

TEST_CASE(config_rejects_bad_values) {
  auto bad = [](const std::string& body) {
    const std::string path = write_temp(body);
    bool threw = false;
    try {
      load_config(path);
    } catch (const std::runtime_error&) {
      threw = true;
    }
    std::filesystem::remove(path);
    return threw;
  };
  CHECK(bad(R"({"labels": {"from_container": ["nonsense"]}})"));
  CHECK(bad(R"({"loki": {"url": "https://loki:3100"}})"));
  CHECK(bad(R"({"podman": {"initial_lookback": "yesterday"}})"));
  CHECK(bad(R"({"podman": {"exclude": ["("]}})"));
  CHECK(bad(R"({"podman": {"timeout": "abc"}})"));
  CHECK(bad(R"({"log_level": 5})"));
  CHECK(bad(R"(not json)"));
  CHECK(!bad(R"({})"));
}

TEST_CASE(env_overrides_apply) {
  setenv("PODLOGS_LOKI_URL", "http://override:3100", 1);
  setenv("PODLOGS_LOKI_BASIC_AUTH", "bob:secret", 1);
  Config cfg = load_config("");
  unsetenv("PODLOGS_LOKI_URL");
  unsetenv("PODLOGS_LOKI_BASIC_AUTH");
  CHECK_EQ(cfg.loki.url, std::string("http://override:3100"));
  CHECK_EQ(cfg.loki.basic_auth_user, std::string("bob"));
  CHECK_EQ(cfg.loki.basic_auth_pass, std::string("secret"));
}

TEST_CASE(identity_builds_labels_from_container_and_config) {
  Config cfg;
  ContainerInfo c;
  c.id = "0123456789abcdef0123456789abcdef";
  c.name = "orders-1";
  c.image = "registry.example.com/team/orders:1.4.2";
  c.image_id = "fedcba9876543210fedcba9876543210";
  c.pod_name = "shop";
  c.labels = {{"io.podman.compose.project", "shop"}, {"io.podman.compose.service", "orders"}};
  auto id = make_identity(cfg, c, "stderr", "host-1", "error");
  CHECK_EQ(id->labels.at("job"), std::string("podlogs"));
  CHECK_EQ(id->labels.at("host"), std::string("host-1"));
  CHECK_EQ(id->labels.at("container_name"), std::string("orders-1"));
  CHECK_EQ(id->labels.at("image_name"), std::string("registry.example.com/team/orders"));
  CHECK_EQ(id->labels.at("image_tag"), std::string("1.4.2"));
  CHECK_EQ(id->labels.at("image_id"), std::string("fedcba987654"));
  CHECK_EQ(id->labels.at("stream"), std::string("stderr"));
  CHECK_EQ(id->labels.at("pod"), std::string("shop"));
  CHECK_EQ(id->labels.at("compose_project"), std::string("shop"));
  CHECK_EQ(id->labels.at("compose_service"), std::string("orders"));
  CHECK(!id->labels.count("level"));
  CHECK_EQ(id->metadata.at("container_id"), std::string("0123456789ab"));
  CHECK_EQ(id->container_id, c.id);

  cfg.labels.level_as_label = true;
  auto with_level = make_identity(cfg, c, "stdout", "host-1", "error");
  CHECK_EQ(with_level->labels.at("level"), std::string("error"));
  CHECK(with_level->key != id->key);
}

TEST_CASE(podman_json_parsing) {
  ContainerInfo c = PodmanClient::parse_container(R"({
    "Id": "abc123", "Names": ["orders"], "Image": "orders:1", "ImageID": "deadbeef",
    "State": "running", "PodName": "", "IsInfra": false, "StartedAt": 1714566896,
    "Labels": {"a": "b", "n": 1}
  })");
  CHECK_EQ(c.id, std::string("abc123"));
  CHECK_EQ(c.name, std::string("orders"));
  CHECK_EQ(c.state, std::string("running"));
  CHECK_EQ(c.started_at, int64_t{1714566896});
  CHECK_EQ(c.labels.size(), size_t{1});

  auto ev = PodmanClient::parse_event(R"({"Type":"container","Action":"start","Actor":{"ID":"abc","Attributes":{"name":"orders","image":"orders:1"}},"time":1})");
  CHECK(ev && ev->type == "container" && ev->action == "start" && ev->id == "abc" && ev->name == "orders");
  auto legacy = PodmanClient::parse_event(R"({"status":"died","id":"abc","from":"orders:1"})");
  CHECK(legacy && legacy->action == "died" && legacy->type == "container");
  CHECK(!PodmanClient::parse_event("garbage"));
}
