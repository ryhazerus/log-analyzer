#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "http.hpp"
#include "util.hpp"

namespace podlogs {

struct ContainerInfo {
  std::string id;
  std::string name;
  std::string image;     // e.g. registry.example.com/team/orders:1.4.2
  std::string image_id;  // full sha256 hex
  std::string state;     // running, exited, ...
  std::string pod_name;
  bool is_infra = false;
  int64_t started_at = 0;  // unix seconds
  std::map<std::string, std::string> labels;
};

struct PodmanEvent {
  std::string type;    // container, image, pod, ...
  std::string action;  // start, died, remove, ...
  std::string id;
  std::string name;
  std::string image;
};

// Client for the podman REST API (libpod flavour) over its unix socket.
class PodmanClient {
 public:
  PodmanClient(http::Endpoint endpoint, std::string api_prefix, int timeout_ms);

  const http::Endpoint& endpoint() const { return endpoint_; }

  // GET /libpod/version -> "4.9.4" (throws on failure)
  std::string version() const;
  std::vector<ContainerInfo> list_containers(bool all) const;
  std::unique_ptr<http::Stream> open_events() const;
  std::unique_ptr<http::Stream> open_logs(const std::string& id, std::optional<util::Nanos> since) const;

  static ContainerInfo parse_container(const std::string& json_text);
  static std::optional<PodmanEvent> parse_event(const std::string& json_line);

  // Finds the podman socket: $CONTAINER_HOST, /run/podman/podman.sock,
  // $XDG_RUNTIME_DIR/podman/podman.sock, /run/user/<uid>/podman/podman.sock.
  static std::optional<std::string> detect_socket();

 private:
  std::string path(const std::string& suffix) const { return api_prefix_ + suffix; }

  http::Endpoint endpoint_;
  std::string api_prefix_;
  int timeout_ms_;
};

}  // namespace podlogs
