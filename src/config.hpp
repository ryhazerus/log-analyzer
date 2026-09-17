#pragma once

#include <map>
#include <string>
#include <vector>

#include "loki.hpp"
#include "parse.hpp"

namespace podlogs {

struct PodmanConfig {
  std::string socket;  // empty = auto-detect
  std::string api_prefix = "/v4.0.0/libpod";
  int timeout_ms = 10000;
  int resync_interval_ms = 15000;
  // Regexes matched against container name and image; empty include = all.
  std::vector<std::string> include;
  std::vector<std::string> exclude = {"^podlogs"};
  bool skip_infra = true;
  // For containers without a checkpoint: "all", or a duration like "1h".
  std::string initial_lookback = "1h";
};

struct LabelsConfig {
  std::map<std::string, std::string> static_labels = {{"job", "podlogs"}};
  // Fields that become Loki stream labels. Valid: host, container_name,
  // container_id, image, image_name, image_tag, image_id, stream, pod.
  std::vector<std::string> from_container = {"host", "container_name", "image_name", "image_tag", "image_id", "stream", "pod"};
  // Fields attached as structured metadata instead (not indexed, no cardinality cost).
  std::vector<std::string> as_metadata = {"container_id"};
  // Container label -> Loki label name, added when the container has that label.
  std::map<std::string, std::string> container_labels = {{"io.podman.compose.project", "compose_project"},
                                                         {"io.podman.compose.service", "compose_service"},
                                                         {"com.docker.compose.project", "compose_project"},
                                                         {"com.docker.compose.service", "compose_service"}};
  // When true, the detected level is a stream label as well as `detected_level` metadata.
  bool level_as_label = false;
};

struct CheckpointConfig {
  std::string path = "/var/lib/podlogs/checkpoint.json";
  int interval_ms = 5000;
};

struct Config {
  std::string log_level = "info";
  std::string hostname;  // empty = gethostname()
  size_t queue_capacity = 50000;
  std::string metrics_listen = "127.0.0.1:9151";
  PodmanConfig podman;
  LokiConfig loki;
  CheckpointConfig checkpoint;
  ParseConfig parse;
  LabelsConfig labels;
};

// Loads the JSON config (comments allowed) from `path` (may be empty for
// defaults), then applies PODLOGS_* environment overrides. Throws
// std::runtime_error with a readable message on invalid input.
Config load_config(const std::string& path);
void apply_env_overrides(Config& cfg);
void validate_config(const Config& cfg);
std::string dump_config(const Config& cfg);

}  // namespace podlogs
