#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <vector>

namespace podlogs {

// Bounded multi-producer / single-consumer queue. Producers block when the
// queue is full, which is how back-pressure reaches the tailers (and, through
// them, podman) when Loki is slow or down.
template <class T>
class BoundedQueue {
 public:
  explicit BoundedQueue(size_t capacity) : capacity_(capacity) {}

  // Returns false if the queue is closed or the timeout elapsed while full.
  bool push(T value, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(mu_);
    if (!not_full_.wait_for(lk, timeout, [&] { return closed_ || items_.size() < capacity_; })) return false;
    if (closed_) return false;
    items_.push_back(std::move(value));
    not_empty_.notify_one();
    return true;
  }

  // Waits up to `timeout` for at least one item, then moves up to `max` items
  // into `out`. Returns the number moved.
  size_t pop_batch(std::vector<T>& out, size_t max, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(mu_);
    if (items_.empty()) not_empty_.wait_for(lk, timeout, [&] { return closed_ || !items_.empty(); });
    size_t n = 0;
    while (!items_.empty() && n < max) {
      out.push_back(std::move(items_.front()));
      items_.pop_front();
      ++n;
    }
    if (n > 0) not_full_.notify_all();
    return n;
  }

  void close() {
    std::lock_guard<std::mutex> lk(mu_);
    closed_ = true;
    not_empty_.notify_all();
    not_full_.notify_all();
  }

  bool closed() const {
    std::lock_guard<std::mutex> lk(mu_);
    return closed_;
  }

  size_t size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return items_.size();
  }

  size_t capacity() const { return capacity_; }

 private:
  const size_t capacity_;
  mutable std::mutex mu_;
  std::condition_variable not_empty_;
  std::condition_variable not_full_;
  std::deque<T> items_;
  bool closed_ = false;
};

}  // namespace podlogs
