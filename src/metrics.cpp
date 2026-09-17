#include "metrics.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <sstream>

#include "log.hpp"
#include "util.hpp"

namespace podlogs {

std::string Metrics::render_prometheus() const {
  std::ostringstream o;
  auto counter = [&](const char* name, const char* help, uint64_t v) {
    o << "# HELP " << name << ' ' << help << "\n# TYPE " << name << " counter\n" << name << ' ' << v << '\n';
  };
  auto gauge = [&](const char* name, const char* help, uint64_t v) {
    o << "# HELP " << name << ' ' << help << "\n# TYPE " << name << " gauge\n" << name << ' ' << v << '\n';
  };
  counter("podlogs_lines_read_total", "Log lines read from podman.", lines_read.load());
  counter("podlogs_bytes_read_total", "Bytes read from podman log streams.", bytes_read.load());
  counter("podlogs_entries_queued_total", "Entries handed to the shipper.", entries_queued.load());
  counter("podlogs_entries_dropped_total", "Entries dropped because the queue stayed full during shutdown.", entries_dropped.load());
  counter("podlogs_entries_shipped_total", "Entries acknowledged by Loki.", entries_shipped.load());
  counter("podlogs_batches_sent_total", "Successful Loki push requests.", batches_sent.load());
  counter("podlogs_batches_dropped_total", "Batches rejected permanently by Loki (4xx).", batches_dropped.load());
  counter("podlogs_push_retries_total", "Retried Loki push requests.", push_retries.load());
  counter("podlogs_push_errors_total", "Failed Loki push attempts (transport or 5xx).", push_errors.load());
  counter("podlogs_tailer_starts_total", "Container log tailers started.", tailer_starts.load());
  counter("podlogs_tailer_errors_total", "Container log tailer failures.", tailer_errors.load());
  counter("podlogs_events_reconnects_total", "Reconnects of the podman events stream.", events_reconnects.load());
  gauge("podlogs_containers_tailing", "Containers currently being tailed.", containers_tailing.load());
  gauge("podlogs_loki_up", "1 if the last Loki push succeeded.", loki_up.load());
  gauge("podlogs_queue_size", "Entries waiting to be shipped.", queue_size ? queue_size() : 0);
  return o.str();
}

MetricsServer::MetricsServer(std::string listen, const Metrics& metrics) : listen_(std::move(listen)), metrics_(metrics) {}

MetricsServer::~MetricsServer() { stop(); }

void MetricsServer::start() {
  if (listen_.empty()) return;
  std::string host = listen_;
  std::string port = "9151";
  const size_t colon = listen_.rfind(':');
  if (colon != std::string::npos) {
    host = listen_.substr(0, colon);
    port = listen_.substr(colon + 1);
  }
  if (host.empty()) host = "0.0.0.0";

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  addrinfo* res = nullptr;
  const int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
  if (rc != 0) {
    LOG_ERROR << "metrics: cannot resolve " << listen_ << ": " << gai_strerror(rc);
    return;
  }
  for (addrinfo* ai = res; ai; ai = ai->ai_next) {
    const int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (::bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && ::listen(fd, 16) == 0) {
      listen_fd_ = fd;
      break;
    }
    ::close(fd);
  }
  freeaddrinfo(res);
  if (listen_fd_ < 0) {
    LOG_ERROR << "metrics: cannot listen on " << listen_ << ": " << std::strerror(errno);
    return;
  }
  LOG_INFO << "metrics: listening on http://" << listen_ << " (/metrics, /healthz)";
  thread_ = std::thread([this] { run(); });
}

void MetricsServer::stop() {
  stop_.store(true);
  if (thread_.joinable()) thread_.join();
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
}

void MetricsServer::run() {
  while (!stop_.load()) {
    pollfd p{listen_fd_, POLLIN, 0};
    const int r = poll(&p, 1, 250);
    if (r <= 0) continue;
    const int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) continue;
    std::string req;
    char buf[4096];
    pollfd rp{fd, POLLIN, 0};
    while (req.find("\r\n\r\n") == std::string::npos && req.size() < 16384) {
      if (poll(&rp, 1, 1000) <= 0) break;
      const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
      if (n <= 0) break;
      req.append(buf, static_cast<size_t>(n));
    }
    std::string body;
    std::string status = "200 OK";
    std::string ctype = "text/plain; version=0.0.4; charset=utf-8";
    if (req.rfind("GET /metrics", 0) == 0) {
      body = metrics_.render_prometheus();
    } else if (req.rfind("GET /healthz", 0) == 0 || req.rfind("GET /health", 0) == 0) {
      body = "ok\n";
      ctype = "text/plain";
    } else {
      status = "404 Not Found";
      body = "not found\n";
      ctype = "text/plain";
    }
    const std::string resp = "HTTP/1.1 " + status + "\r\nContent-Type: " + ctype +
                             "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    size_t off = 0;
    while (off < resp.size()) {
      const ssize_t n = ::send(fd, resp.data() + off, resp.size() - off, 0);
      if (n <= 0) break;
      off += static_cast<size_t>(n);
    }
    ::close(fd);
  }
}

}  // namespace podlogs
