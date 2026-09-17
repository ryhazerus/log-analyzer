#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>

#include "util.hpp"

namespace podlogs {

// Remembers, per container, the timestamp of the newest entry Loki has
// acknowledged, so a restart resumes where it left off. Persisted as JSON with
// an atomic rename.
class Checkpoint {
 public:
  explicit Checkpoint(std::string path);

  // Returns false (and starts empty) if the file is missing or unreadable.
  bool load();
  bool save();
  bool dirty() const;

  std::optional<util::Nanos> get(const std::string& container_id) const;
  // Advances the checkpoint if `ts` is newer than what is stored.
  void advance(const std::string& container_id, util::Nanos ts, const std::string& name);
  void remove(const std::string& container_id);
  // Drops every container not in `keep` (e.g. containers podman no longer knows).
  void retain(const std::set<std::string>& keep);
  size_t size() const;
  const std::string& path() const { return path_; }

 private:
  struct Entry {
    util::Nanos ts = 0;
    std::string name;
  };
  std::string path_;
  mutable std::mutex mu_;
  std::map<std::string, Entry> entries_;
  bool dirty_ = false;
};

}  // namespace podlogs
