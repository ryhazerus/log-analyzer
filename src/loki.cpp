#include "loki.hpp"

#include <algorithm>
#include <chrono>
#include <unordered_map>

#include "log.hpp"
#include "nlohmann/json.hpp"

namespace podlogs {

using nlohmann::json;

std::string build_push_payload(const std::vector<LogEntry>& entries) {
  // Group by stream identity, keeping first-seen order for readability.
  std::unordered_map<std::string, size_t> index;
  std::vector<std::vector<const LogEntry*>> groups;
  std::vector<const StreamIdentity*> identities;
  for (const LogEntry& e : entries) {
    auto [it, inserted] = index.emplace(e.stream->key, groups.size());
    if (inserted) {
      groups.emplace_back();
      identities.push_back(e.stream.get());
    }
    groups[it->second].push_back(&e);
  }

  json streams = json::array();
  for (size_t g = 0; g < groups.size(); ++g) {
    auto& group = groups[g];
    std::stable_sort(group.begin(), group.end(), [](const LogEntry* a, const LogEntry* b) { return a->ts < b->ts; });
    json values = json::array();
    for (const LogEntry* e : group) {
      json value = json::array({std::to_string(e->ts), e->line});
      json meta = identities[g]->metadata;
      if (!e->level.empty()) meta["detected_level"] = e->level;
      if (!e->logger.empty()) meta["logger"] = e->logger;
      if (!meta.empty()) value.push_back(std::move(meta));
      values.push_back(std::move(value));
    }
    streams.push_back({{"stream", identities[g]->labels}, {"values", std::move(values)}});
  }
  const json doc = {{"streams", std::move(streams)}};
  // Log lines are arbitrary bytes; replace invalid UTF-8 instead of throwing.
  return doc.dump(-1, ' ', false, json::error_handler_t::replace);
}

LokiClient::LokiClient(LokiConfig cfg, BoundedQueue<LogEntry>& queue, Metrics& metrics, AckFn on_ack)
    : cfg_(std::move(cfg)), queue_(queue), metrics_(metrics), on_ack_(std::move(on_ack)) {
  auto ep = http::parse_endpoint(cfg_.url);
  if (!ep) throw std::runtime_error("invalid loki url: " + cfg_.url + " (expected http://host:port)");
  endpoint_ = *ep;
}

LokiClient::~LokiClient() {
  if (thread_.joinable()) stop(0);
}

void LokiClient::start() {
  thread_ = std::thread([this] { run(); });
}

void LokiClient::stop(int drain_timeout_ms) {
  (void)drain_timeout_ms;
  stopping_.store(true);
  if (thread_.joinable()) thread_.join();
}

std::string LokiClient::check_ready() const {
  http::Request req;
  req.path = endpoint_.base_path + "/ready";
  try {
    const http::Response resp = http::request(endpoint_, req, cfg_.timeout_ms);
    std::string body = resp.body;
    while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.pop_back();
    return "HTTP " + std::to_string(resp.status) + " " + body.substr(0, 120);
  } catch (const std::exception& e) {
    return std::string("unreachable: ") + e.what();
  }
}

void LokiClient::run() {
  using clock = std::chrono::steady_clock;
  std::vector<LogEntry> batch;
  std::vector<LogEntry> got;
  size_t batch_bytes = 0;
  clock::time_point batch_start = clock::now();

  while (true) {
    got.clear();
    const size_t n = queue_.pop_batch(got, 1000, std::chrono::milliseconds(100));
    const bool was_empty = batch.empty();
    for (auto& e : got) {
      batch_bytes += e.line.size() + 64;
      batch.push_back(std::move(e));
    }
    if (was_empty && !batch.empty()) batch_start = clock::now();

    const bool waited = clock::now() - batch_start >= std::chrono::milliseconds(cfg_.batch_wait_ms);
    const bool full = batch_bytes >= cfg_.batch_bytes || batch.size() >= cfg_.batch_entries;
    const bool draining = queue_.closed() || stopping_.load();

    if (!batch.empty() && (full || waited || draining)) {
      send(batch);
      batch.clear();
      batch_bytes = 0;
      batch_start = clock::now();
    }
    if (draining && n == 0 && queue_.size() == 0) break;
  }
}

bool LokiClient::send(std::vector<LogEntry>& batch) {
  const std::string body = build_push_payload(batch);
  int attempt = 0;
  int backoff_ms = 500;
  while (true) {
    std::string error;
    if (push_once(body, error)) {
      metrics_.batches_sent++;
      metrics_.entries_shipped += batch.size();
      metrics_.loki_up.store(1);
      if (on_ack_) {
        std::unordered_map<std::string, std::pair<const StreamIdentity*, util::Nanos>> newest;
        for (const auto& e : batch) {
          auto& slot = newest[e.stream->container_id];
          const util::Nanos ack = e.ack_ts > 0 ? e.ack_ts : e.ts;
          if (ack > slot.second) slot = {e.stream.get(), ack};
        }
        for (const auto& [id, slot] : newest) on_ack_(id, slot.first->container_name, slot.second);
      }
      return true;
    }
    metrics_.loki_up.store(0);
    if (error.rfind("permanent:", 0) == 0) {
      metrics_.batches_dropped++;
      LOG_ERROR << "loki: dropping batch of " << batch.size() << " entries: " << error.substr(10);
      return false;
    }
    metrics_.push_errors++;
    ++attempt;
    const bool give_up = (cfg_.max_retries >= 0 && attempt > cfg_.max_retries) || (stopping_.load() && attempt > 1);
    if (give_up) {
      metrics_.batches_dropped++;
      LOG_ERROR << "loki: giving up on batch of " << batch.size() << " entries after " << attempt << " attempts: " << error;
      return false;
    }
    metrics_.push_retries++;
    if (attempt == 1 || attempt % 10 == 0) {
      LOG_WARN << "loki: push failed (attempt " << attempt << "): " << error << "; retrying in " << backoff_ms << "ms";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
    backoff_ms = std::min(backoff_ms * 2, cfg_.backoff_max_ms);
  }
}

bool LokiClient::push_once(const std::string& body, std::string& error) {
  http::Request req;
  req.method = "POST";
  req.path = endpoint_.base_path + "/loki/api/v1/push";
  req.headers["Content-Type"] = "application/json";
  if (!cfg_.tenant.empty()) req.headers["X-Scope-OrgID"] = cfg_.tenant;
  if (!cfg_.basic_auth_user.empty()) {
    req.headers["Authorization"] = "Basic " + util::base64(cfg_.basic_auth_user + ":" + cfg_.basic_auth_pass);
  }
  req.body = body;
  http::Response resp;
  try {
    resp = http::request(endpoint_, req, cfg_.timeout_ms);
  } catch (const std::exception& e) {
    error = e.what();
    return false;
  }
  if (resp.status >= 200 && resp.status < 300) return true;
  std::string snippet = resp.body.substr(0, 300);
  for (auto& c : snippet) {
    if (c == '\n' || c == '\r') c = ' ';
  }
  const std::string what = "HTTP " + std::to_string(resp.status) + " " + resp.reason + (snippet.empty() ? "" : ": " + snippet);
  if (resp.status == 429 || resp.status >= 500) {
    error = what;
  } else {
    error = "permanent:" + what;
  }
  return false;
}

}  // namespace podlogs
