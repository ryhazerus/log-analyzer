#include "podman.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include "log.hpp"
#include "nlohmann/json.hpp"

namespace podlogs {

using nlohmann::json;

namespace {

std::string str_or(const json& j, const char* key, const std::string& def = "") {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_string()) return def;
  return it->get<std::string>();
}

ContainerInfo container_from_json(const json& j) {
  ContainerInfo c;
  c.id = str_or(j, "Id");
  if (const auto names = j.find("Names"); names != j.end() && names->is_array() && !names->empty() && (*names)[0].is_string()) {
    c.name = (*names)[0].get<std::string>();
    if (!c.name.empty() && c.name[0] == '/') c.name.erase(0, 1);
  }
  c.image = str_or(j, "Image");
  c.image_id = str_or(j, "ImageID");
  c.state = str_or(j, "State");
  c.pod_name = str_or(j, "PodName");
  if (const auto it = j.find("IsInfra"); it != j.end() && it->is_boolean()) c.is_infra = it->get<bool>();
  if (const auto it = j.find("StartedAt"); it != j.end() && it->is_number()) c.started_at = it->get<int64_t>();
  if (const auto it = j.find("Labels"); it != j.end() && it->is_object()) {
    for (const auto& [k, v] : it->items()) {
      if (v.is_string()) c.labels[k] = v.get<std::string>();
    }
  }
  return c;
}

bool is_socket(const std::string& path) {
  struct stat st{};
  return ::stat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
}

}  // namespace

PodmanClient::PodmanClient(http::Endpoint endpoint, std::string api_prefix, int timeout_ms)
    : endpoint_(std::move(endpoint)), api_prefix_(std::move(api_prefix)), timeout_ms_(timeout_ms) {}

std::string PodmanClient::version() const {
  http::Request req;
  req.path = path("/version");
  const http::Response resp = http::request(endpoint_, req, timeout_ms_);
  if (resp.status != 200) throw http::Error("GET " + req.path + " -> HTTP " + std::to_string(resp.status));
  const json j = json::parse(resp.body, nullptr, false);
  if (!j.is_object()) throw http::Error("unexpected /version response");
  return str_or(j, "Version", "unknown");
}

std::vector<ContainerInfo> PodmanClient::list_containers(bool all) const {
  http::Request req;
  req.path = path(std::string("/containers/json?all=") + (all ? "true" : "false"));
  const http::Response resp = http::request(endpoint_, req, timeout_ms_);
  if (resp.status != 200) {
    throw http::Error("GET " + req.path + " -> HTTP " + std::to_string(resp.status) + " " + resp.body.substr(0, 200));
  }
  const json j = json::parse(resp.body, nullptr, false);
  if (!j.is_array()) throw http::Error("unexpected containers/json response");
  std::vector<ContainerInfo> out;
  out.reserve(j.size());
  for (const auto& item : j) {
    if (item.is_object()) out.push_back(container_from_json(item));
  }
  return out;
}

std::unique_ptr<http::Stream> PodmanClient::open_events() const {
  http::Request req;
  req.path = path("/events?stream=true&filters=") + util::url_encode(R"({"type":["container"]})");
  return http::open_stream(endpoint_, req, timeout_ms_);
}

std::unique_ptr<http::Stream> PodmanClient::open_logs(const std::string& id, std::optional<util::Nanos> since) const {
  http::Request req;
  req.path = path("/containers/" + id + "/logs?follow=true&stdout=true&stderr=true&timestamps=true");
  if (since) req.path += "&since=" + util::url_encode(util::format_rfc3339(*since));
  return http::open_stream(endpoint_, req, timeout_ms_);
}

ContainerInfo PodmanClient::parse_container(const std::string& json_text) {
  const json j = json::parse(json_text, nullptr, false);
  if (!j.is_object()) return {};
  return container_from_json(j);
}

std::optional<PodmanEvent> PodmanClient::parse_event(const std::string& json_line) {
  const json j = json::parse(json_line, nullptr, false);
  if (!j.is_object()) return std::nullopt;
  PodmanEvent ev;
  ev.type = str_or(j, "Type");
  ev.action = str_or(j, "Action", str_or(j, "status"));
  if (const auto actor = j.find("Actor"); actor != j.end() && actor->is_object()) {
    ev.id = str_or(*actor, "ID");
    if (const auto attrs = actor->find("Attributes"); attrs != actor->end() && attrs->is_object()) {
      ev.name = str_or(*attrs, "name");
      ev.image = str_or(*attrs, "image");
    }
  }
  if (ev.id.empty()) ev.id = str_or(j, "id");
  if (ev.image.empty()) ev.image = str_or(j, "from");
  if (ev.type.empty() && !ev.id.empty()) ev.type = "container";
  if (ev.action.empty() || ev.id.empty()) return std::nullopt;
  return ev;
}

std::optional<std::string> PodmanClient::detect_socket() {
  const std::string from_env = util::getenv_or("CONTAINER_HOST", "");
  if (!from_env.empty()) {
    if (auto ep = http::parse_endpoint(from_env); ep && ep->is_unix() && is_socket(ep->unix_path)) return ep->unix_path;
  }
  std::vector<std::string> candidates = {"/run/podman/podman.sock"};
  const std::string xdg = util::getenv_or("XDG_RUNTIME_DIR", "");
  if (!xdg.empty()) candidates.push_back(xdg + "/podman/podman.sock");
  candidates.push_back("/run/user/" + std::to_string(getuid()) + "/podman/podman.sock");
  for (const auto& c : candidates) {
    if (is_socket(c)) return c;
  }
  return std::nullopt;
}

}  // namespace podlogs
