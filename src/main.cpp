#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <thread>

#include "checkpoint.hpp"
#include "config.hpp"
#include "log.hpp"
#include "loki.hpp"
#include "metrics.hpp"
#include "podman.hpp"
#include "tailer.hpp"
#include "version.hpp"

namespace {

std::atomic<bool> g_stop{false};
static_assert(std::atomic<bool>::is_always_lock_free);

void on_signal(int) { g_stop.store(true); }

void usage() {
  std::printf(R"(podlogs %s - ships podman container logs to Loki

Usage: podlogs [options]

  -c, --config PATH        JSON config file (comments allowed)
      --podman-socket PATH podman API socket (default: auto-detect)
      --loki-url URL       Loki base URL (default: http://127.0.0.1:3100)
      --checkpoint PATH    checkpoint file (default: /var/lib/podlogs/checkpoint.json)
      --log-level LEVEL    debug|info|warn|error
      --check              validate config, test podman + Loki connectivity, list
                           the containers that would be tailed, then exit
      --print-config       print the effective configuration as JSON and exit
  -v, --version            print version and exit
  -h, --help               this help

Environment overrides: PODLOGS_PODMAN_SOCKET, PODLOGS_LOKI_URL, PODLOGS_LOKI_TENANT,
PODLOGS_LOKI_BASIC_AUTH (user:pass), PODLOGS_CHECKPOINT_PATH, PODLOGS_LOG_LEVEL,
PODLOGS_METRICS_LISTEN, PODLOGS_HOSTNAME, PODLOGS_INITIAL_LOOKBACK
)",
              PODLOGS_VERSION);
}

}  // namespace

int main(int argc, char** argv) {
  using namespace podlogs;

  std::string config_path;
  std::string podman_socket, loki_url, checkpoint_path, log_level;
  bool check_only = false, print_config = false;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto need = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s requires a value\n", name);
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "-c" || a == "--config") config_path = need("--config");
    else if (a == "--podman-socket") podman_socket = need("--podman-socket");
    else if (a == "--loki-url") loki_url = need("--loki-url");
    else if (a == "--checkpoint") checkpoint_path = need("--checkpoint");
    else if (a == "--log-level") log_level = need("--log-level");
    else if (a == "--check") check_only = true;
    else if (a == "--print-config") print_config = true;
    else if (a == "-v" || a == "--version") {
      std::printf("podlogs %s\n", PODLOGS_VERSION);
      return 0;
    } else if (a == "-h" || a == "--help") {
      usage();
      return 0;
    } else {
      std::fprintf(stderr, "unknown option: %s\n\n", a.c_str());
      usage();
      return 2;
    }
  }
  if (config_path.empty()) config_path = util::getenv_or("PODLOGS_CONFIG", "");

  Config cfg;
  try {
    cfg = load_config(config_path);
    if (!podman_socket.empty()) cfg.podman.socket = podman_socket;
    if (!loki_url.empty()) cfg.loki.url = loki_url;
    if (!checkpoint_path.empty()) cfg.checkpoint.path = checkpoint_path;
    if (!log_level.empty()) cfg.log_level = log_level;
    validate_config(cfg);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "config error: %s\n", e.what());
    return 2;
  }

  log::Level lvl;
  if (!log::parse_level(cfg.log_level, lvl)) {
    std::fprintf(stderr, "config error: invalid log_level '%s'\n", cfg.log_level.c_str());
    return 2;
  }
  log::set_level(lvl);
  if (cfg.hostname.empty()) cfg.hostname = util::hostname();

  if (cfg.podman.socket.empty()) {
    auto detected = PodmanClient::detect_socket();
    if (!detected) {
      std::fprintf(stderr,
                   "no podman socket found. Enable it with\n"
                   "  systemctl enable --now podman.socket          (rootful)\n"
                   "  systemctl --user enable --now podman.socket   (rootless)\n"
                   "or set podman.socket / PODLOGS_PODMAN_SOCKET / --podman-socket.\n");
      return 2;
    }
    cfg.podman.socket = *detected;
  }

  if (print_config) {
    std::printf("%s\n", dump_config(cfg).c_str());
    return 0;
  }

  auto podman_ep = http::parse_endpoint(cfg.podman.socket);
  if (!podman_ep) {
    std::fprintf(stderr, "config error: invalid podman.socket '%s'\n", cfg.podman.socket.c_str());
    return 2;
  }

  std::signal(SIGPIPE, SIG_IGN);
  struct sigaction sa{};
  sa.sa_handler = on_signal;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  LOG_INFO << "podlogs " << PODLOGS_VERSION << " starting (host=" << cfg.hostname << ")";

  PodmanClient podman(*podman_ep, cfg.podman.api_prefix, cfg.podman.timeout_ms);
  Metrics metrics;
  BoundedQueue<LogEntry> queue(cfg.queue_capacity);
  metrics.queue_size = [&queue] { return static_cast<uint64_t>(queue.size()); };
  Checkpoint checkpoint(cfg.checkpoint.path);
  LineParser parser(cfg.parse);
  TailContext ctx{cfg, podman, parser, checkpoint, queue, metrics, cfg.hostname};

  std::unique_ptr<LokiClient> loki;
  try {
    loki = std::make_unique<LokiClient>(
        cfg.loki, queue, metrics,
        [&checkpoint](const std::string& id, const std::string& name, util::Nanos ts) { checkpoint.advance(id, ts, name); });
  } catch (const std::exception& e) {
    std::fprintf(stderr, "config error: %s\n", e.what());
    return 2;
  }

  // Connectivity check (also what --check reports).
  bool podman_ok = false;
  try {
    const std::string ver = podman.version();
    LOG_INFO << "podman " << ver << " via " << podman_ep->describe();
    podman_ok = true;
  } catch (const std::exception& e) {
    LOG_ERROR << "podman: cannot reach API at " << podman_ep->describe() << ": " << e.what();
  }

  if (check_only) {
    std::printf("podman socket : %s (%s)\n", cfg.podman.socket.c_str(), podman_ok ? "ok" : "FAILED");
    std::printf("loki          : %s (%s)\n", cfg.loki.url.c_str(), loki->check_ready().c_str());
    std::printf("checkpoint    : %s\n", cfg.checkpoint.path.c_str());
    if (!podman_ok) return 1;
    TailManager mgr(ctx);
    try {
      auto containers = podman.list_containers(false);
      size_t n = 0;
      for (const auto& c : containers) {
        if (c.state != "running") continue;
        const bool sel = mgr.selected(c);
        n += sel ? 1 : 0;
        const util::ImageRef ref = util::parse_image_ref(c.image);
        std::printf("  %s %-12s %-40s image_name=%s image_tag=%s image_id=%s%s\n", sel ? "+" : "-",
                    util::short_id(c.id).c_str(), c.name.c_str(), ref.name.c_str(), ref.tag.c_str(),
                    util::short_id(c.image_id).c_str(), sel ? "" : "  (excluded)");
      }
      std::printf("%zu of %zu running containers would be tailed\n", n, containers.size());
    } catch (const std::exception& e) {
      std::printf("listing containers failed: %s\n", e.what());
      return 1;
    }
    return 0;
  }

  if (!podman_ok) LOG_WARN << "podman: will keep retrying";

  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(cfg.checkpoint.path).parent_path(), ec);
  if (checkpoint.load()) {
    LOG_INFO << "checkpoint: loaded " << checkpoint.size() << " containers from " << cfg.checkpoint.path;
  } else {
    LOG_INFO << "checkpoint: none at " << cfg.checkpoint.path << ", using initial_lookback=" << cfg.podman.initial_lookback;
  }
  // Forget containers podman no longer knows about.
  try {
    std::set<std::string> known;
    for (const auto& c : podman.list_containers(true)) known.insert(c.id);
    checkpoint.retain(known);
  } catch (const std::exception&) {
  }

  MetricsServer metrics_server(cfg.metrics_listen, metrics);
  metrics_server.start();
  loki->start();

  std::thread checkpoint_thread([&] {
    while (!g_stop.load()) {
      for (int i = 0; i < cfg.checkpoint.interval_ms / 100 && !g_stop.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      if (checkpoint.dirty()) checkpoint.save();
    }
  });

  {
    TailManager manager(ctx);
    manager.run(g_stop);
  }

  LOG_INFO << "draining " << queue.size() << " queued entries to Loki";
  queue.close();
  loki->stop(0);
  checkpoint_thread.join();
  checkpoint.save();
  metrics_server.stop();
  LOG_INFO << "shutdown complete: shipped=" << metrics.entries_shipped.load() << " dropped=" << metrics.entries_dropped.load()
           << " batches_dropped=" << metrics.batches_dropped.load();
  return 0;
}
