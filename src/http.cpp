#include "http.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "util.hpp"

namespace podlogs::http {

namespace {

std::string errno_str(const std::string& what) { return what + ": " + std::strerror(errno); }

void set_nonblocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void set_socket_options(int fd) {
#ifdef SO_NOSIGPIPE
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
  (void)fd;
}

int send_flags() {
#ifdef MSG_NOSIGNAL
  return MSG_NOSIGNAL;
#else
  return 0;
#endif
}

// Waits for the non-blocking connect to complete. Throws a bare reason; the
// caller adds the "connect to <where>" context.
void finish_connect(int fd, int timeout_ms) {
  pollfd p{fd, POLLOUT, 0};
  const int r = poll(&p, 1, timeout_ms);
  if (r == 0) throw Error("timed out");
  if (r < 0) throw Error(errno_str("poll"));
  int err = 0;
  socklen_t len = sizeof err;
  if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) throw Error(errno_str("getsockopt"));
  if (err != 0) throw Error(std::strerror(err));
}

}  // namespace

std::string Endpoint::host_header() const {
  if (is_unix()) return "localhost";
  if (port == 80) return host;
  return host + ":" + std::to_string(port);
}

std::string Endpoint::describe() const {
  if (is_unix()) return "unix://" + unix_path;
  return "http://" + host + ":" + std::to_string(port) + base_path;
}

std::optional<Endpoint> parse_endpoint(std::string_view url) {
  Endpoint ep;
  if (util::starts_with(url, "unix://")) {
    ep.unix_path = std::string(url.substr(7));
  } else if (util::starts_with(url, "unix:")) {
    ep.unix_path = std::string(url.substr(5));
  } else if (util::starts_with(url, "/")) {
    ep.unix_path = std::string(url);
  } else if (util::starts_with(url, "http://")) {
    std::string_view rest = url.substr(7);
    const size_t slash = rest.find('/');
    std::string_view hostport = slash == std::string_view::npos ? rest : rest.substr(0, slash);
    if (slash != std::string_view::npos) {
      ep.base_path = std::string(rest.substr(slash));
      while (!ep.base_path.empty() && ep.base_path.back() == '/') ep.base_path.pop_back();
    }
    if (hostport.empty()) return std::nullopt;
    if (hostport.front() == '[') {  // IPv6 literal
      const size_t close = hostport.find(']');
      if (close == std::string_view::npos) return std::nullopt;
      ep.host = std::string(hostport.substr(1, close - 1));
      hostport = hostport.substr(close + 1);
      if (!hostport.empty()) {
        if (hostport.front() != ':') return std::nullopt;
        ep.port = std::atoi(std::string(hostport.substr(1)).c_str());
      }
    } else {
      const size_t colon = hostport.rfind(':');
      if (colon == std::string_view::npos) {
        ep.host = std::string(hostport);
      } else {
        ep.host = std::string(hostport.substr(0, colon));
        ep.port = std::atoi(std::string(hostport.substr(colon + 1)).c_str());
      }
    }
    if (ep.host.empty() || ep.port <= 0 || ep.port > 65535) return std::nullopt;
  } else {
    return std::nullopt;
  }
  if (ep.is_unix() && ep.unix_path.empty()) return std::nullopt;
  return ep;
}

std::string Request::serialize(const Endpoint& ep) const {
  std::string out;
  out.reserve(256 + body.size());
  out += method;
  out += ' ';
  out += path;
  out += " HTTP/1.1\r\nHost: ";
  out += ep.host_header();
  out += "\r\nUser-Agent: podlogs\r\nConnection: close\r\n";
  if (!body.empty() || method == "POST" || method == "PUT") {
    out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  }
  for (const auto& [k, v] : headers) {
    out += k;
    out += ": ";
    out += v;
    out += "\r\n";
  }
  out += "\r\n";
  out += body;
  return out;
}

std::optional<std::string> Response::header(const std::string& name) const {
  const auto it = headers.find(util::to_lower(name));
  if (it == headers.end()) return std::nullopt;
  return it->second;
}

// ---------------------------------------------------------------------------
// Connection

Connection::~Connection() {
  const int fd = fd_.exchange(-1);
  if (fd >= 0) ::close(fd);
}

std::unique_ptr<Connection> Connection::open(const Endpoint& ep, int timeout_ms) {
  if (ep.is_unix()) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (ep.unix_path.size() >= sizeof(addr.sun_path)) throw Error("unix socket path too long: " + ep.unix_path);
    std::strncpy(addr.sun_path, ep.unix_path.c_str(), sizeof(addr.sun_path) - 1);
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) throw Error(errno_str("socket"));
    set_socket_options(fd);
    set_nonblocking(fd);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0 && errno != EINPROGRESS) {
      const std::string err = errno_str("connect to " + ep.unix_path);
      ::close(fd);
      throw Error(err);
    }
    try {
      finish_connect(fd, timeout_ms);
    } catch (const Error& e) {
      ::close(fd);
      throw Error("connect to " + ep.unix_path + ": " + e.what());
    }
    return std::unique_ptr<Connection>(new Connection(fd));
  }

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  const int rc = getaddrinfo(ep.host.c_str(), std::to_string(ep.port).c_str(), &hints, &res);
  if (rc != 0) throw Error("resolve " + ep.host + ": " + gai_strerror(rc));
  std::string last_error = "no addresses";
  for (addrinfo* ai = res; ai; ai = ai->ai_next) {
    const int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) {
      last_error = errno_str("socket");
      continue;
    }
    set_socket_options(fd);
    set_nonblocking(fd);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (::connect(fd, ai->ai_addr, ai->ai_addrlen) < 0 && errno != EINPROGRESS) {
      last_error = errno_str("connect");
      ::close(fd);
      continue;
    }
    try {
      finish_connect(fd, timeout_ms);
    } catch (const Error& e) {
      last_error = e.what();
      ::close(fd);
      continue;
    }
    freeaddrinfo(res);
    return std::unique_ptr<Connection>(new Connection(fd));
  }
  freeaddrinfo(res);
  throw Error("connect to " + ep.host + ":" + std::to_string(ep.port) + ": " + last_error);
}

void Connection::send_all(std::string_view data, int timeout_ms) {
  size_t off = 0;
  while (off < data.size()) {
    const int fd = fd_.load();
    if (fd < 0) throw Error("send on closed connection");
    const ssize_t n = ::send(fd, data.data() + off, data.size() - off, send_flags());
    if (n > 0) {
      off += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      pollfd p{fd, POLLOUT, 0};
      const int r = poll(&p, 1, timeout_ms);
      if (r == 0) throw Error("send timed out");
      if (r < 0 && errno != EINTR) throw Error(errno_str("poll"));
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    throw Error(errno_str("send"));
  }
}

int Connection::read_some(char* buf, size_t len, int timeout_ms) {
  while (true) {
    const int fd = fd_.load();
    if (fd < 0) return 0;
    pollfd p{fd, POLLIN, 0};
    const int r = poll(&p, 1, timeout_ms);
    if (r == 0) return -1;
    if (r < 0) {
      if (errno == EINTR) continue;
      throw Error(errno_str("poll"));
    }
    const ssize_t n = ::recv(fd, buf, len, 0);
    if (n > 0) return static_cast<int>(n);
    if (n == 0) return 0;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
    if (errno == ECONNRESET || errno == EBADF) return 0;
    throw Error(errno_str("recv"));
  }
}

void Connection::shutdown() {
  const int fd = fd_.load();
  if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
}

// ---------------------------------------------------------------------------
// BodyDecoder

BodyDecoder::BodyDecoder(Framing framing, size_t content_length) : framing_(framing), remaining_(content_length) {
  if (framing_ == Framing::ContentLength && content_length == 0) done_ = true;
}

bool BodyDecoder::feed(std::string_view raw, std::string& out) {
  if (done_) return true;
  switch (framing_) {
    case Framing::UntilClose:
      out.append(raw);
      return false;
    case Framing::ContentLength: {
      const size_t take = std::min(remaining_, raw.size());
      out.append(raw.substr(0, take));
      remaining_ -= take;
      if (remaining_ == 0) done_ = true;
      return done_;
    }
    case Framing::Chunked:
      break;
  }

  size_t pos = 0;
  while (pos < raw.size() && !done_) {
    switch (chunk_state_) {
      case ChunkState::Size: {
        const size_t nl = raw.find('\n', pos);
        if (nl == std::string_view::npos) {
          line_.append(raw.substr(pos));
          pos = raw.size();
          break;
        }
        line_.append(raw.substr(pos, nl - pos));
        pos = nl + 1;
        std::string_view sz = util::trim(line_);
        const size_t semi = sz.find(';');
        if (semi != std::string_view::npos) sz = sz.substr(0, semi);
        if (sz.empty()) {  // tolerate a stray CRLF between chunks
          line_.clear();
          break;
        }
        size_t n = 0;
        for (char c : sz) {
          int d;
          if (c >= '0' && c <= '9') d = c - '0';
          else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
          else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
          else throw Error("malformed chunk size: " + std::string(sz));
          n = n * 16 + static_cast<size_t>(d);
        }
        line_.clear();
        if (n == 0) {
          chunk_state_ = ChunkState::Trailer;
        } else {
          remaining_ = n;
          chunk_state_ = ChunkState::Data;
        }
        break;
      }
      case ChunkState::Data: {
        const size_t take = std::min(remaining_, raw.size() - pos);
        out.append(raw.substr(pos, take));
        pos += take;
        remaining_ -= take;
        if (remaining_ == 0) chunk_state_ = ChunkState::DataCrlf;
        break;
      }
      case ChunkState::DataCrlf: {
        const size_t nl = raw.find('\n', pos);
        if (nl == std::string_view::npos) {
          pos = raw.size();
          break;
        }
        pos = nl + 1;
        chunk_state_ = ChunkState::Size;
        break;
      }
      case ChunkState::Trailer: {
        const size_t nl = raw.find('\n', pos);
        if (nl == std::string_view::npos) {
          line_.append(raw.substr(pos));
          pos = raw.size();
          break;
        }
        line_.append(raw.substr(pos, nl - pos));
        pos = nl + 1;
        const bool blank = util::trim(line_).empty();
        line_.clear();
        if (blank) done_ = true;
        break;
      }
    }
  }
  return done_;
}

// ---------------------------------------------------------------------------
// Head parsing

size_t parse_response_head(std::string_view data, Response& out) {
  size_t end = data.find("\r\n\r\n");
  size_t sep_len = 4;
  if (end == std::string_view::npos) {
    end = data.find("\n\n");
    sep_len = 2;
    if (end == std::string_view::npos) return 0;
  }
  const std::string_view head = data.substr(0, end);
  size_t line_start = 0;
  bool first = true;
  while (line_start <= head.size()) {
    size_t nl = head.find('\n', line_start);
    if (nl == std::string_view::npos) nl = head.size();
    std::string_view line = util::trim(head.substr(line_start, nl - line_start));
    line_start = nl + 1;
    if (line.empty()) {
      if (first) throw Error("empty status line");
      continue;
    }
    if (first) {
      first = false;
      if (!util::starts_with(line, "HTTP/")) throw Error("not an HTTP response: " + std::string(line.substr(0, 40)));
      const size_t sp = line.find(' ');
      if (sp == std::string_view::npos) throw Error("malformed status line");
      const std::string_view rest = util::trim(line.substr(sp + 1));
      const size_t sp2 = rest.find(' ');
      const std::string_view code = rest.substr(0, sp2);
      out.status = std::atoi(std::string(code).c_str());
      if (out.status <= 0) throw Error("malformed status code");
      out.reason = sp2 == std::string_view::npos ? "" : std::string(util::trim(rest.substr(sp2 + 1)));
      continue;
    }
    const size_t colon = line.find(':');
    if (colon == std::string_view::npos) continue;
    out.headers[util::to_lower(util::trim(line.substr(0, colon)))] = std::string(util::trim(line.substr(colon + 1)));
  }
  return end + sep_len;
}

BodyDecoder decoder_for(const Response& head, const std::string& method) {
  if (method == "HEAD" || head.status == 204 || head.status == 304 || (head.status >= 100 && head.status < 200)) {
    return BodyDecoder(BodyDecoder::Framing::ContentLength, 0);
  }
  if (auto te = head.header("transfer-encoding"); te && util::to_lower(*te).find("chunked") != std::string::npos) {
    return BodyDecoder(BodyDecoder::Framing::Chunked);
  }
  if (auto cl = head.header("content-length")) {
    return BodyDecoder(BodyDecoder::Framing::ContentLength, static_cast<size_t>(std::strtoull(cl->c_str(), nullptr, 10)));
  }
  return BodyDecoder(BodyDecoder::Framing::UntilClose);
}

// ---------------------------------------------------------------------------
// Stream

Stream::Stream(std::unique_ptr<Connection> conn, Response head, BodyDecoder decoder, std::string leftover)
    : conn_(std::move(conn)), head_(std::move(head)), decoder_(decoder), leftover_(std::move(leftover)) {}

int Stream::read(std::string& out, int timeout_ms) {
  char buf[64 * 1024];
  while (true) {
    if (!leftover_.empty()) {
      const size_t before = out.size();
      decoder_.feed(leftover_, out);
      leftover_.clear();
      if (out.size() > before) return static_cast<int>(out.size() - before);
    }
    if (decoder_.done() || eof_) return 0;
    const int n = conn_->read_some(buf, sizeof buf, timeout_ms);
    if (n < 0) return -1;
    if (n == 0) {
      eof_ = true;
      return 0;
    }
    const size_t before = out.size();
    decoder_.feed(std::string_view(buf, static_cast<size_t>(n)), out);
    if (out.size() > before) return static_cast<int>(out.size() - before);
  }
}

void Stream::cancel() { conn_->shutdown(); }

std::string Stream::drain(size_t max_bytes, int timeout_ms) {
  std::string body;
  while (body.size() < max_bytes) {
    const int n = read(body, timeout_ms);
    if (n <= 0) break;
  }
  if (body.size() > max_bytes) body.resize(max_bytes);
  return body;
}

// ---------------------------------------------------------------------------
// Entry points

namespace {

struct HeadResult {
  Response head;
  std::string leftover;
};

HeadResult read_head(Connection& conn, int timeout_ms) {
  std::string buf;
  char tmp[16 * 1024];
  while (true) {
    Response head;
    const size_t consumed = buf.empty() ? 0 : parse_response_head(buf, head);
    if (consumed > 0) return {std::move(head), buf.substr(consumed)};
    if (buf.size() > 256 * 1024) throw Error("response head too large");
    const int n = conn.read_some(tmp, sizeof tmp, timeout_ms);
    if (n < 0) throw Error("timed out waiting for response");
    if (n == 0) throw Error("connection closed before response head");
    buf.append(tmp, static_cast<size_t>(n));
  }
}

}  // namespace

Response request(const Endpoint& ep, const Request& req, int timeout_ms) {
  auto conn = Connection::open(ep, timeout_ms);
  conn->send_all(req.serialize(ep), timeout_ms);
  HeadResult hr = read_head(*conn, timeout_ms);
  BodyDecoder dec = decoder_for(hr.head, req.method);
  Stream stream(std::move(conn), std::move(hr.head), dec, std::move(hr.leftover));
  Response resp = stream.head();
  while (true) {
    const int n = stream.read(resp.body, timeout_ms);
    if (n == 0) break;
    if (n < 0) throw Error("timed out reading response body");
    if (resp.body.size() > 64 * 1024 * 1024) throw Error("response body too large");
  }
  return resp;
}

std::unique_ptr<Stream> open_stream(const Endpoint& ep, const Request& req, int timeout_ms) {
  auto conn = Connection::open(ep, timeout_ms);
  conn->send_all(req.serialize(ep), timeout_ms);
  HeadResult hr = read_head(*conn, timeout_ms);
  BodyDecoder dec = decoder_for(hr.head, req.method);
  return std::make_unique<Stream>(std::move(conn), std::move(hr.head), dec, std::move(hr.leftover));
}

}  // namespace podlogs::http
