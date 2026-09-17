#pragma once

#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// A deliberately small HTTP/1.1 server for tests. Handlers run on a thread
// per connection and write the response themselves, which makes it easy to
// emulate podman's long-lived chunked streams.
namespace testing {

struct FakeRequest {
  std::string method;
  std::string path;  // includes query
  std::map<std::string, std::string> headers;
  std::string body;
};

class FakeConn {
 public:
  FakeConn(int fd, const std::atomic<bool>& stopping) : fd_(fd), stopping_(stopping) {}
  ~FakeConn() {
    if (fd_ >= 0) ::close(fd_);
  }

  bool write(std::string_view data) {
    size_t off = 0;
    while (off < data.size()) {
      const ssize_t n = ::send(fd_, data.data() + off, data.size() - off, 0);
      if (n <= 0) return false;
      off += static_cast<size_t>(n);
    }
    return true;
  }

  void respond(int status, const std::string& body, const std::string& content_type = "application/json") {
    write("HTTP/1.1 " + std::to_string(status) + " X\r\nContent-Type: " + content_type +
          "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
  }

  void begin_chunked(int status = 200, const std::string& content_type = "application/json") {
    write("HTTP/1.1 " + std::to_string(status) + " OK\r\nContent-Type: " + content_type +
          "\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
  }

  bool chunk(std::string_view data) {
    char hdr[32];
    std::snprintf(hdr, sizeof hdr, "%zx\r\n", data.size());
    return write(hdr) && write(data) && write("\r\n");
  }

  void end_chunked() { write("0\r\n\r\n"); }

  // True once the peer closed the socket or the server is stopping.
  bool peer_closed() const {
    pollfd p{fd_, POLLIN, 0};
    if (poll(&p, 1, 0) > 0 && (p.revents & (POLLHUP | POLLERR))) return true;
    if (p.revents & POLLIN) {
      char b;
      if (::recv(fd_, &b, 1, MSG_PEEK) == 0) return true;
    }
    return false;
  }

  // Blocks until the server stops or the client goes away.
  void hold_open() const {
    while (!stopping_.load() && !peer_closed()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  bool stopping() const { return stopping_.load(); }

 private:
  int fd_;
  const std::atomic<bool>& stopping_;
};

class FakeServer {
 public:
  using Handler = std::function<void(const FakeRequest&, FakeConn&)>;

  static std::unique_ptr<FakeServer> listen_unix(const std::string& path) {
    ::unlink(path.c_str());
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(fd, 64) != 0) {
      ::close(fd);
      throw std::runtime_error("fake server: cannot listen on " + path);
    }
    auto s = std::unique_ptr<FakeServer>(new FakeServer(fd));
    s->path_ = path;
    return s;
  }

  static std::unique_ptr<FakeServer> listen_tcp() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(fd, 64) != 0) {
      ::close(fd);
      throw std::runtime_error("fake server: cannot listen on loopback");
    }
    socklen_t len = sizeof addr;
    getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    auto s = std::unique_ptr<FakeServer>(new FakeServer(fd));
    s->port_ = ntohs(addr.sin_port);
    return s;
  }

  ~FakeServer() { stop(); }

  void set_handler(Handler h) {
    std::lock_guard<std::mutex> lk(mu_);
    handler_ = std::move(h);
  }

  void start() {
    accept_thread_ = std::thread([this] { accept_loop(); });
  }

  void stop() {
    if (stopping_.exchange(true)) return;
    if (accept_thread_.joinable()) accept_thread_.join();
    for (auto& t : conn_threads_) {
      if (t.joinable()) t.join();
    }
    conn_threads_.clear();
    if (listen_fd_ >= 0) ::close(listen_fd_);
    listen_fd_ = -1;
    if (!path_.empty()) ::unlink(path_.c_str());
  }

  int port() const { return port_; }
  const std::string& path() const { return path_; }
  std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }

  std::vector<std::string> requests() const {
    std::lock_guard<std::mutex> lk(mu_);
    return request_log_;
  }

 private:
  explicit FakeServer(int fd) : listen_fd_(fd) {}

  void accept_loop() {
    while (!stopping_.load()) {
      pollfd p{listen_fd_, POLLIN, 0};
      if (poll(&p, 1, 50) <= 0) continue;
      const int fd = ::accept(listen_fd_, nullptr, nullptr);
      if (fd < 0) continue;
      std::lock_guard<std::mutex> lk(mu_);
      conn_threads_.emplace_back([this, fd] { serve(fd); });
    }
  }

  void serve(int fd) {
    FakeConn conn(fd, stopping_);
    std::string raw;
    char buf[8192];
    size_t head_end = std::string::npos;
    while (head_end == std::string::npos) {
      pollfd p{fd, POLLIN, 0};
      if (poll(&p, 1, 2000) <= 0) return;
      const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
      if (n <= 0) return;
      raw.append(buf, static_cast<size_t>(n));
      head_end = raw.find("\r\n\r\n");
    }
    FakeRequest req;
    {
      const std::string head = raw.substr(0, head_end);
      size_t pos = 0;
      bool first = true;
      while (pos < head.size()) {
        size_t nl = head.find("\r\n", pos);
        if (nl == std::string::npos) nl = head.size();
        const std::string line = head.substr(pos, nl - pos);
        pos = nl + 2;
        if (first) {
          first = false;
          const size_t sp1 = line.find(' ');
          const size_t sp2 = line.find(' ', sp1 + 1);
          req.method = line.substr(0, sp1);
          req.path = line.substr(sp1 + 1, sp2 - sp1 - 1);
          continue;
        }
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string k = line.substr(0, colon);
        for (auto& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string v = line.substr(colon + 1);
        while (!v.empty() && v.front() == ' ') v.erase(0, 1);
        req.headers[k] = v;
      }
    }
    size_t content_length = 0;
    if (auto it = req.headers.find("content-length"); it != req.headers.end()) content_length = std::stoul(it->second);
    req.body = raw.substr(head_end + 4);
    while (req.body.size() < content_length) {
      pollfd p{fd, POLLIN, 0};
      if (poll(&p, 1, 2000) <= 0) return;
      const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
      if (n <= 0) return;
      req.body.append(buf, static_cast<size_t>(n));
    }
    Handler h;
    {
      std::lock_guard<std::mutex> lk(mu_);
      request_log_.push_back(req.method + " " + req.path);
      h = handler_;
    }
    if (h) h(req, conn);
    else conn.respond(404, "no handler");
  }

  int listen_fd_;
  int port_ = 0;
  std::string path_;
  std::atomic<bool> stopping_{false};
  std::thread accept_thread_;
  mutable std::mutex mu_;
  std::vector<std::thread> conn_threads_;
  std::vector<std::string> request_log_;
  Handler handler_;
};

// Builds one docker-style multiplexed log frame with a timestamp prefix.
inline std::string log_frame(int stream_type, const std::string& ts, const std::string& line) {
  const std::string payload = ts + " " + line + "\n";
  std::string frame(8, '\0');
  frame[0] = static_cast<char>(stream_type);
  const uint32_t len = static_cast<uint32_t>(payload.size());
  frame[4] = static_cast<char>((len >> 24) & 0xff);
  frame[5] = static_cast<char>((len >> 16) & 0xff);
  frame[6] = static_cast<char>((len >> 8) & 0xff);
  frame[7] = static_cast<char>(len & 0xff);
  return frame + payload;
}

}  // namespace testing
