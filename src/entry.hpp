#pragma once

#include <map>
#include <memory>
#include <string>

#include "util.hpp"

namespace podlogs {

// Everything that is constant for one (container, stdout|stderr) pair. Shared
// by all entries of that stream so per-line cost stays at one string copy.
struct StreamIdentity {
  std::string container_id;                     // full id, used for checkpointing
  std::string container_name;
  std::map<std::string, std::string> labels;    // Loki stream labels (sorted)
  std::map<std::string, std::string> metadata;  // Loki structured metadata attached to every entry
  std::string key;                              // canonical label serialization, used for batching
};

struct LogEntry {
  std::shared_ptr<const StreamIdentity> stream;
  util::Nanos ts = 0;       // entry timestamp (first line)
  util::Nanos ack_ts = 0;   // newest line timestamp covered by this entry; drives the checkpoint
  std::string line;    // raw text as logged (multi-line entries joined with '\n')
  std::string level;   // normalized level if detected, else empty
  std::string logger;  // logger name if the JSON carried one, else empty
};

}  // namespace podlogs
