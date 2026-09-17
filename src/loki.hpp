#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "entry.hpp"
#include "http.hpp"
#include "metrics.hpp"
#include "queue.hpp"

namespace podlogs {

struct LokiConfig {
  std::string url = "http://127.0.0.1:3100";
  int timeout_ms = 15000;
  int batch_wait_ms = 1000;
  size_t batch_bytes = 1024 * 1024;
  size_t batch_entries = 5000;
  // -1 retries transient failures forever (back-pressure instead of data loss).
  int max_retries = -1;
  int backoff_max_ms = 10000;
  std::string tenant;           // X-Scope-OrgID
  std::string basic_auth_user;  // optional HTTP basic auth
  std::string basic_auth_pass;
};

// Builds the JSON body for POST /loki/api/v1/push. Exposed for tests.
std::string build_push_payload(const std::vector<LogEntry>& entries);

class LokiClient {
 public:
  // on_ack is called per container with the newest timestamp Loki accepted.
  using AckFn = std::function<void(const std::string& container_id, const std::string& container_name, util::Nanos ts)>;

  LokiClient(LokiConfig cfg, BoundedQueue<LogEntry>& queue, Metrics& metrics, AckFn on_ack);
  ~LokiClient();

  void start();
  // Drains what it can within `drain_timeout_ms` and returns.
  void stop(int drain_timeout_ms);

  // One-off connectivity check (GET /ready). Returns a human readable status.
  std::string check_ready() const;

 private:
  void run();
  bool send(std::vector<LogEntry>& batch);
  bool push_once(const std::string& body, std::string& error);

  LokiConfig cfg_;
  http::Endpoint endpoint_;
  BoundedQueue<LogEntry>& queue_;
  Metrics& metrics_;
  AckFn on_ack_;
  std::atomic<bool> stopping_{false};
  std::thread thread_;
};

}  // namespace podlogs
