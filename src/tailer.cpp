#include "tailer.hpp"

#include <chrono>
#include <set>

#include "demux.hpp"
#include "log.hpp"

namespace podlogs {

namespace {

std::string identity_key(const std::map<std::string, std::string>& labels) {
  std::string key;
  for (const auto& [k, v] : labels) {
    key += k;
    key += '=';
    key += v;
    key += '\x1f';
  }
  return key;
}

void sleep_unless(const std::atomic<bool>& flag, int ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (!flag.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

}  // namespace

std::shared_ptr<StreamIdentity> make_identity(const Config& cfg, const ContainerInfo& c, const std::string& stream,
                                              const std::string& hostname, const std::string& level) {
  const util::ImageRef ref = util::parse_image_ref(c.image);
  const std::map<std::string, std::string> fields = {
      {"host", hostname},
      {"container_name", c.name},
      {"container_id", util::short_id(c.id)},
      {"image", c.image},
      {"image_name", ref.name},
      {"image_tag", ref.tag},
      {"image_id", util::short_id(c.image_id)},
      {"stream", stream},
      {"pod", c.pod_name},
  };
  auto id = std::make_shared<StreamIdentity>();
  id->container_id = c.id;
  id->container_name = c.name;
  for (const auto& [k, v] : cfg.labels.static_labels) id->labels[k] = v;
  for (const auto& f : cfg.labels.from_container) {
    const auto it = fields.find(f);
    if (it != fields.end() && !it->second.empty()) id->labels[f] = it->second;
  }
  for (const auto& f : cfg.labels.as_metadata) {
    const auto it = fields.find(f);
    if (it != fields.end() && !it->second.empty()) id->metadata[f] = it->second;
  }
  for (const auto& [ckey, lname] : cfg.labels.container_labels) {
    const auto it = c.labels.find(ckey);
    if (it != c.labels.end() && !it->second.empty()) id->labels[util::sanitize_label_name(lname)] = it->second;
  }
  if (cfg.labels.level_as_label && !level.empty()) id->labels["level"] = level;
  id->key = identity_key(id->labels);
  return id;
}

// ---------------------------------------------------------------------------
// ContainerTailer

ContainerTailer::ContainerTailer(TailContext ctx, ContainerInfo container, std::optional<util::Nanos> since)
    : ctx_(std::move(ctx)), container_(std::move(container)), since_(since) {}

ContainerTailer::~ContainerTailer() {
  stop();
  join();
}

void ContainerTailer::start() {
  ctx_.metrics.tailer_starts++;
  thread_ = std::thread([this] { run(); });
}

void ContainerTailer::stop() {
  stop_.store(true);
  std::lock_guard<std::mutex> lk(stream_mu_);
  if (current_stream_) current_stream_->cancel();
}

void ContainerTailer::join() {
  if (thread_.joinable()) thread_.join();
}

std::shared_ptr<const StreamIdentity> ContainerTailer::identity(const std::string& stream, const std::string& level) {
  const std::string key = ctx_.config.labels.level_as_label ? stream + "/" + level : stream;
  auto it = identities_.find(key);
  if (it == identities_.end()) {
    it = identities_.emplace(key, make_identity(ctx_.config, container_, stream, ctx_.hostname, level)).first;
  }
  return it->second;
}

bool ContainerTailer::enqueue(LogEntry entry) {
  while (!ctx_.queue.push(std::move(entry), std::chrono::milliseconds(500))) {
    if (stop_.load() || ctx_.queue.closed()) {
      ctx_.metrics.entries_dropped++;
      return false;
    }
    // Queue full: Loki is behind. Keep waiting; podman buffers on disk.
  }
  ctx_.metrics.entries_queued++;
  return true;
}

void ContainerTailer::process(std::vector<RawLine>& lines) {
  for (RawLine& l : lines) {
    const ParsedLine parsed = ctx_.parser.parse(l.text);
    LogEntry e;
    e.stream = identity(l.stream, parsed.level);
    e.ts = l.ts;
    e.ack_ts = std::max(l.ts, l.ts_last);
    e.line = std::move(l.text);
    e.level = parsed.level;
    e.logger = parsed.logger;
    const util::Nanos ts = e.ack_ts;
    if (!enqueue(std::move(e))) break;
    ctx_.metrics.lines_read++;
    if (ts > last_ts_.load()) last_ts_.store(ts);
  }
  lines.clear();
}

void ContainerTailer::run() {
  const std::string label = container_.name + " (" + util::short_id(container_.id) + ")";
  const int flush_ms = std::max(50, ctx_.config.parse.multiline.flush_timeout_ms);
  int backoff_ms = 500;

  while (!stop_.load()) {
    std::unique_ptr<http::Stream> stream;
    try {
      stream = ctx_.podman.open_logs(container_.id, since_);
    } catch (const std::exception& e) {
      ctx_.metrics.tailer_errors++;
      LOG_WARN << "tail " << label << ": cannot open log stream: " << e.what() << "; retrying in " << backoff_ms << "ms";
      sleep_unless(stop_, backoff_ms);
      backoff_ms = std::min(backoff_ms * 2, 10000);
      continue;
    }
    if (stream->status() != 200) {
      const std::string body = stream->drain(300, 1000);
      ctx_.metrics.tailer_errors++;
      if (stream->status() == 404) {
        LOG_INFO << "tail " << label << ": container is gone";
      } else {
        LOG_WARN << "tail " << label << ": log stream returned HTTP " << stream->status() << " " << body;
      }
      break;  // the manager re-creates the tailer on the next sync if the container is still running
    }
    {
      std::lock_guard<std::mutex> lk(stream_mu_);
      current_stream_ = stream.get();
    }
    backoff_ms = 500;

    Demuxer demux;
    MultilineAggregator agg_out(ctx_.parser);
    MultilineAggregator agg_err(ctx_.parser);
    std::string chunk;
    std::vector<RawLine> lines;
    std::vector<RawLine> ready;

    auto flush_stale = [&] {
      const util::Nanos now = util::now_nanos();
      agg_out.flush_if_stale(now, ready);
      agg_err.flush_if_stale(now, ready);
    };

    while (!stop_.load()) {
      chunk.clear();
      const int n = stream->read(chunk, flush_ms);
      if (n < 0) {
        flush_stale();
        process(ready);
        continue;
      }
      if (n == 0) break;
      ctx_.metrics.bytes_read += chunk.size();
      demux.feed(chunk, lines);
      for (RawLine& l : lines) {
        if (since_ && l.ts < *since_) continue;
        (l.stream == "stderr" ? agg_err : agg_out).push(std::move(l), ready);
      }
      lines.clear();
      flush_stale();
      process(ready);
    }
    demux.finish(lines);
    for (RawLine& l : lines) (l.stream == "stderr" ? agg_err : agg_out).push(std::move(l), ready);
    lines.clear();
    agg_out.flush(ready);
    agg_err.flush(ready);
    process(ready);
    {
      std::lock_guard<std::mutex> lk(stream_mu_);
      current_stream_ = nullptr;
    }
    stream.reset();
    if (!stop_.load()) LOG_DEBUG << "tail " << label << ": log stream ended";
    break;
  }
  finished_.store(true);
}

// ---------------------------------------------------------------------------
// TailManager

TailManager::TailManager(TailContext ctx) : ctx_(std::move(ctx)) {
  for (const auto& p : ctx_.config.podman.include) include_.emplace_back(p, std::regex::ECMAScript | std::regex::optimize);
  for (const auto& p : ctx_.config.podman.exclude) exclude_.emplace_back(p, std::regex::ECMAScript | std::regex::optimize);
}

TailManager::~TailManager() {
  if (events_thread_.joinable()) events_thread_.join();
}

bool TailManager::selected(const ContainerInfo& c) const {
  if (ctx_.config.podman.skip_infra && c.is_infra) return false;
  auto matches_any = [&](const std::vector<std::regex>& res) {
    for (const auto& re : res) {
      if (std::regex_search(c.name, re) || std::regex_search(c.image, re)) return true;
    }
    return false;
  };
  if (!include_.empty() && !matches_any(include_)) return false;
  if (matches_any(exclude_)) return false;
  return true;
}

size_t TailManager::active() const {
  std::lock_guard<std::mutex> lk(mu_);
  return tailers_.size();
}

void TailManager::request_sync() {
  {
    std::lock_guard<std::mutex> lk(wake_mu_);
    sync_requested_ = true;
  }
  wake_.notify_all();
}

std::optional<util::Nanos> TailManager::since_for(const ContainerInfo& c) const {
  if (auto cp = ctx_.checkpoint.get(c.id)) return *cp;
  const std::string& lookback = ctx_.config.podman.initial_lookback;
  if (lookback == "all") return std::nullopt;
  const auto ms = util::parse_duration_ms(lookback).value_or(3600 * 1000);
  return util::now_nanos() - ms * util::kNanosPerMs;
}

void TailManager::sync() {
  std::vector<ContainerInfo> containers;
  try {
    containers = ctx_.podman.list_containers(false);
  } catch (const std::exception& e) {
    LOG_WARN << "podman: listing containers failed: " << e.what();
    return;
  }

  std::lock_guard<std::mutex> lk(mu_);
  std::set<std::string> running;
  for (const ContainerInfo& c : containers) {
    if (c.state != "running" || !selected(c)) continue;
    running.insert(c.id);
    auto it = tailers_.find(c.id);
    if (it != tailers_.end()) {
      if (!it->second->finished()) continue;
      it->second->join();
      tailers_.erase(it);
    }
    const auto since = since_for(c);
    auto tailer = std::make_unique<ContainerTailer>(ctx_, c, since);
    LOG_INFO << "tailing " << c.name << " (" << util::short_id(c.id) << ") image=" << c.image
             << (since ? " since=" + util::format_rfc3339(*since) : " since=beginning");
    tailer->start();
    tailers_[c.id] = std::move(tailer);
  }
  for (auto it = tailers_.begin(); it != tailers_.end();) {
    if (running.count(it->first)) {
      ++it;
      continue;
    }
    if (!it->second->finished()) {
      LOG_INFO << "container " << it->second->container().name << " (" << util::short_id(it->first) << ") is no longer running";
      it->second->stop();
    }
    it->second->join();
    it = tailers_.erase(it);
  }
  ctx_.metrics.containers_tailing.store(tailers_.size());
}

void TailManager::handle_event(const PodmanEvent& ev) {
  if (ev.type != "container") return;
  if (ev.action == "start" || ev.action == "restart" || ev.action == "unpause") {
    LOG_DEBUG << "event: container " << ev.name << " " << ev.action;
    request_sync();
  } else if (ev.action == "remove") {
    ctx_.checkpoint.remove(ev.id);
    request_sync();
  } else if (ev.action == "died" || ev.action == "die" || ev.action == "stop") {
    request_sync();
  }
}

void TailManager::events_loop(const std::atomic<bool>& stop_flag) {
  int backoff_ms = 1000;
  bool first = true;
  while (!stop_flag.load()) {
    std::unique_ptr<http::Stream> stream;
    try {
      stream = ctx_.podman.open_events();
    } catch (const std::exception& e) {
      LOG_WARN << "podman: events stream unavailable: " << e.what() << "; retrying in " << backoff_ms << "ms";
      sleep_unless(stop_flag, backoff_ms);
      backoff_ms = std::min(backoff_ms * 2, 30000);
      continue;
    }
    if (stream->status() != 200) {
      LOG_WARN << "podman: events stream returned HTTP " << stream->status() << " " << stream->drain(200, 1000);
      sleep_unless(stop_flag, backoff_ms);
      backoff_ms = std::min(backoff_ms * 2, 30000);
      continue;
    }
    {
      std::lock_guard<std::mutex> lk(events_mu_);
      events_stream_ = stream.get();
    }
    if (!first) {
      ctx_.metrics.events_reconnects++;
      LOG_INFO << "podman: events stream reconnected";
      request_sync();
    }
    first = false;
    backoff_ms = 1000;

    std::string buf;
    while (!stop_flag.load()) {
      const int n = stream->read(buf, 1000);
      if (n < 0) continue;
      if (n == 0) break;
      size_t start = 0;
      while (true) {
        const size_t nl = buf.find('\n', start);
        if (nl == std::string::npos) break;
        const std::string line = buf.substr(start, nl - start);
        start = nl + 1;
        if (auto ev = PodmanClient::parse_event(line)) handle_event(*ev);
      }
      buf.erase(0, start);
    }
    {
      std::lock_guard<std::mutex> lk(events_mu_);
      events_stream_ = nullptr;
    }
    stream.reset();
    if (!stop_flag.load()) {
      LOG_WARN << "podman: events stream ended; reconnecting";
      sleep_unless(stop_flag, backoff_ms);
    }
  }
}

void TailManager::run(const std::atomic<bool>& stop_flag) {
  events_thread_ = std::thread([this, &stop_flag] { events_loop(stop_flag); });

  using clock = std::chrono::steady_clock;
  auto last_sync = clock::time_point::min();
  const auto interval = std::chrono::milliseconds(ctx_.config.podman.resync_interval_ms);

  while (!stop_flag.load()) {
    bool do_sync;
    {
      std::unique_lock<std::mutex> lk(wake_mu_);
      wake_.wait_for(lk, std::chrono::milliseconds(250), [&] { return sync_requested_ || stop_flag.load(); });
      do_sync = sync_requested_ || (clock::now() - last_sync >= interval);
      sync_requested_ = false;
    }
    if (stop_flag.load()) break;
    if (do_sync) {
      sync();
      last_sync = clock::now();
    }
  }

  {
    std::lock_guard<std::mutex> lk(events_mu_);
    if (events_stream_) events_stream_->cancel();
  }
  if (events_thread_.joinable()) events_thread_.join();

  std::lock_guard<std::mutex> lk(mu_);
  LOG_INFO << "stopping " << tailers_.size() << " tailers";
  for (auto& [id, t] : tailers_) t->stop();
  for (auto& [id, t] : tailers_) t->join();
  tailers_.clear();
  ctx_.metrics.containers_tailing.store(0);
}

}  // namespace podlogs
