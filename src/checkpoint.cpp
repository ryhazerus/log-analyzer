#include "checkpoint.hpp"

#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <sstream>

#include "log.hpp"
#include "nlohmann/json.hpp"

namespace podlogs {

using nlohmann::json;

Checkpoint::Checkpoint(std::string path) : path_(std::move(path)) {}

bool Checkpoint::load() {
  std::ifstream in(path_);
  if (!in) return false;
  std::stringstream ss;
  ss << in.rdbuf();
  const json j = json::parse(ss.str(), nullptr, false);
  if (!j.is_object() || !j.contains("containers") || !j["containers"].is_object()) {
    LOG_WARN << "checkpoint: " << path_ << " is not a valid checkpoint file, starting fresh";
    return false;
  }
  std::lock_guard<std::mutex> lk(mu_);
  entries_.clear();
  for (const auto& [id, v] : j["containers"].items()) {
    if (!v.is_object() || !v.contains("ts") || !v["ts"].is_number_integer()) continue;
    Entry e;
    e.ts = v["ts"].get<util::Nanos>();
    if (v.contains("name") && v["name"].is_string()) e.name = v["name"].get<std::string>();
    entries_[id] = std::move(e);
  }
  dirty_ = false;
  return true;
}

bool Checkpoint::save() {
  json containers = json::object();
  {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& [id, e] : entries_) {
      containers[id] = {{"ts", e.ts}, {"name", e.name}, {"time", util::format_rfc3339(e.ts)}};
    }
    dirty_ = false;
  }
  const json doc = {{"version", 1}, {"containers", containers}};
  const std::string tmp = path_ + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) {
      LOG_ERROR << "checkpoint: cannot write " << tmp;
      return false;
    }
    out << doc.dump(1, ' ', false, json::error_handler_t::replace) << '\n';
    if (!out) {
      LOG_ERROR << "checkpoint: write to " << tmp << " failed";
      return false;
    }
  }
  if (std::rename(tmp.c_str(), path_.c_str()) != 0) {
    LOG_ERROR << "checkpoint: rename " << tmp << " -> " << path_ << " failed";
    return false;
  }
  return true;
}

bool Checkpoint::dirty() const {
  std::lock_guard<std::mutex> lk(mu_);
  return dirty_;
}

std::optional<util::Nanos> Checkpoint::get(const std::string& container_id) const {
  std::lock_guard<std::mutex> lk(mu_);
  const auto it = entries_.find(container_id);
  if (it == entries_.end()) return std::nullopt;
  return it->second.ts;
}

void Checkpoint::advance(const std::string& container_id, util::Nanos ts, const std::string& name) {
  std::lock_guard<std::mutex> lk(mu_);
  Entry& e = entries_[container_id];
  if (ts > e.ts) {
    e.ts = ts;
    dirty_ = true;
  }
  if (e.name != name && !name.empty()) {
    e.name = name;
    dirty_ = true;
  }
}

void Checkpoint::remove(const std::string& container_id) {
  std::lock_guard<std::mutex> lk(mu_);
  if (entries_.erase(container_id) > 0) dirty_ = true;
}

void Checkpoint::retain(const std::set<std::string>& keep) {
  std::lock_guard<std::mutex> lk(mu_);
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (keep.count(it->first) == 0) {
      it = entries_.erase(it);
      dirty_ = true;
    } else {
      ++it;
    }
  }
}

size_t Checkpoint::size() const {
  std::lock_guard<std::mutex> lk(mu_);
  return entries_.size();
}

}  // namespace podlogs
