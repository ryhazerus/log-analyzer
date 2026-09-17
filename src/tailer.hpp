#pragma once

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "checkpoint.hpp"
#include "config.hpp"
#include "entry.hpp"
#include "http.hpp"
#include "metrics.hpp"
#include "parse.hpp"
#include "podman.hpp"
#include "queue.hpp"

namespace podlogs {

struct TailContext {
  const Config& config;
  const PodmanClient& podman;
  const LineParser& parser;
  Checkpoint& checkpoint;
  BoundedQueue<LogEntry>& queue;
  Metrics& metrics;
  std::string hostname;
};

// Builds the label/metadata sets for one container stream. Exposed for tests.
std::shared_ptr<StreamIdentity> make_identity(const Config& cfg, const ContainerInfo& c, const std::string& stream,
                                              const std::string& hostname, const std::string& level);

// Follows one container's log stream on its own thread until the stream ends
// (container stopped) or stop() is called.
class ContainerTailer {
 public:
  ContainerTailer(TailContext ctx, ContainerInfo container, std::optional<util::Nanos> since);
  ~ContainerTailer();

  void start();
  void stop();
  void join();
  bool finished() const { return finished_.load(); }
  const ContainerInfo& container() const { return container_; }
  util::Nanos last_ts() const { return last_ts_.load(); }

 private:
  void run();
  void process(std::vector<RawLine>& lines);
  bool enqueue(LogEntry entry);
  std::shared_ptr<const StreamIdentity> identity(const std::string& stream, const std::string& level);

  TailContext ctx_;
  ContainerInfo container_;
  std::optional<util::Nanos> since_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> finished_{false};
  std::atomic<util::Nanos> last_ts_{0};
  std::mutex stream_mu_;
  http::Stream* current_stream_ = nullptr;
  std::map<std::string, std::shared_ptr<const StreamIdentity>> identities_;
  std::thread thread_;
};

// Discovers containers (initial sweep + podman events + periodic resync) and
// owns one ContainerTailer per running container.
class TailManager {
 public:
  explicit TailManager(TailContext ctx);
  ~TailManager();

  // Blocks until `stop_flag` becomes true.
  void run(const std::atomic<bool>& stop_flag);

  // True if the container passes include/exclude/infra filters.
  bool selected(const ContainerInfo& c) const;
  size_t active() const;

 private:
  void sync();
  void events_loop(const std::atomic<bool>& stop_flag);
  void handle_event(const PodmanEvent& ev);
  void request_sync();
  std::optional<util::Nanos> since_for(const ContainerInfo& c) const;

  TailContext ctx_;
  std::vector<std::regex> include_;
  std::vector<std::regex> exclude_;
  mutable std::mutex mu_;
  std::map<std::string, std::unique_ptr<ContainerTailer>> tailers_;
  std::condition_variable wake_;
  std::mutex wake_mu_;
  bool sync_requested_ = true;
  std::mutex events_mu_;
  http::Stream* events_stream_ = nullptr;
  std::thread events_thread_;
};

}  // namespace podlogs
