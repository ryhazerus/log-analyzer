#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace podlogs {

struct Metrics {
  std::atomic<uint64_t> lines_read{0};
  std::atomic<uint64_t> bytes_read{0};
  std::atomic<uint64_t> entries_queued{0};
  std::atomic<uint64_t> entries_dropped{0};
  std::atomic<uint64_t> entries_shipped{0};
  std::atomic<uint64_t> batches_sent{0};
  std::atomic<uint64_t> batches_dropped{0};
  std::atomic<uint64_t> push_retries{0};
  std::atomic<uint64_t> push_errors{0};
  std::atomic<uint64_t> tailer_starts{0};
  std::atomic<uint64_t> tailer_errors{0};
  std::atomic<uint64_t> events_reconnects{0};
  std::atomic<uint64_t> containers_tailing{0};
  std::atomic<uint64_t> loki_up{0};

  // Live values that are not counters (queue depth) are read through this.
  std::function<uint64_t()> queue_size;

  std::string render_prometheus() const;
};

// Serves GET /metrics and GET /healthz on a small blocking HTTP server.
class MetricsServer {
 public:
  MetricsServer(std::string listen, const Metrics& metrics);
  ~MetricsServer();
  void start();
  void stop();

 private:
  void run();
  std::string listen_;
  const Metrics& metrics_;
  int listen_fd_ = -1;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

}  // namespace podlogs
