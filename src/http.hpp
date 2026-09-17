#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

// Minimal HTTP/1.1 client over unix or TCP sockets. It covers exactly what the
// collector needs: request/response calls (Loki push, podman list) and
// long-lived streaming responses (podman events and log follow). Keeping this
// in-tree, instead of libcurl, is what lets the release binaries be fully
// static with no TLS/zlib/nghttp2 dependency chain.
namespace podlogs::http {

struct Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct Endpoint {
  std::string unix_path;  // set for unix sockets
  std::string host;       // set for TCP
  int port = 80;
  std::string base_path;  // path prefix taken from the URL, e.g. "/loki" for http://h/loki

  bool is_unix() const { return !unix_path.empty(); }
  std::string host_header() const;
  std::string describe() const;
};

// Accepts "unix:///run/podman/podman.sock", "unix:/path", "/abs/path" and
// "http://host[:port][/base]". Returns nullopt for anything else (incl. https).
std::optional<Endpoint> parse_endpoint(std::string_view url);

struct Request {
  std::string method = "GET";
  std::string path = "/";
  std::map<std::string, std::string> headers;  // Host, Content-Length, Connection are added automatically
  std::string body;

  std::string serialize(const Endpoint& ep) const;
};

struct Response {
  int status = 0;
  std::string reason;
  std::map<std::string, std::string> headers;  // lowercase keys
  std::string body;

  std::optional<std::string> header(const std::string& name) const;
};

class Connection {
 public:
  ~Connection();
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  static std::unique_ptr<Connection> open(const Endpoint& ep, int timeout_ms);

  void send_all(std::string_view data, int timeout_ms);
  // Returns >0 bytes read, 0 on EOF, -1 on timeout. Throws Error on failure.
  int read_some(char* buf, size_t len, int timeout_ms);
  // Thread-safe: wakes up a reader blocked in read_some() with EOF.
  void shutdown();
  int fd() const { return fd_.load(); }

 private:
  explicit Connection(int fd) : fd_(fd) {}
  std::atomic<int> fd_;
};

// Incremental response body decoder.
class BodyDecoder {
 public:
  enum class Framing { Chunked, ContentLength, UntilClose };

  explicit BodyDecoder(Framing framing, size_t content_length = 0);

  // Consumes raw socket bytes and appends decoded body bytes to `out`.
  // Returns true once the body is complete.
  bool feed(std::string_view raw, std::string& out);
  bool done() const { return done_; }
  Framing framing() const { return framing_; }

 private:
  enum class ChunkState { Size, Data, DataCrlf, Trailer };
  Framing framing_;
  bool done_ = false;
  size_t remaining_ = 0;
  ChunkState chunk_state_ = ChunkState::Size;
  std::string line_;
};

// Parses a response head. Returns bytes consumed, or 0 if the head is not yet
// complete. Throws Error on malformed input.
size_t parse_response_head(std::string_view data, Response& out);

// Chooses the body framing from the response headers.
BodyDecoder decoder_for(const Response& head, const std::string& method);

// A streaming response: the head has been read, the body is delivered
// incrementally through read().
class Stream {
 public:
  Stream(std::unique_ptr<Connection> conn, Response head, BodyDecoder decoder, std::string leftover);

  int status() const { return head_.status; }
  const Response& head() const { return head_; }

  // Appends decoded body bytes to `out`. Returns >0 bytes appended, 0 at the
  // end of the body (or EOF), -1 if nothing arrived within timeout_ms.
  int read(std::string& out, int timeout_ms);

  // Thread-safe: makes a blocked read() return 0.
  void cancel();

  // Reads the rest of the body (bounded). Handy for error responses.
  std::string drain(size_t max_bytes, int timeout_ms);

 private:
  std::unique_ptr<Connection> conn_;
  Response head_;
  BodyDecoder decoder_;
  std::string leftover_;
  bool eof_ = false;
};

Response request(const Endpoint& ep, const Request& req, int timeout_ms);
std::unique_ptr<Stream> open_stream(const Endpoint& ep, const Request& req, int timeout_ms);

}  // namespace podlogs::http
