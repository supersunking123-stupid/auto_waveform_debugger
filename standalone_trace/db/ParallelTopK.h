// ParallelTopK.h — bounded top-k selection over a large index range, scanned in parallel.
//
// The result equals "collect every emitted item, std::sort by `less`, keep the first `limit`"
// (items that compare equal are interchangeable for the callers), but without materializing all
// items and without a global sort. Each worker keeps only its own best `limit` items in a
// bounded max-heap; the per-worker results are merged and sorted at the end.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <iterator>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace rtl_trace {

// Below this many elements the scan runs inline on the calling thread (no thread spawn).
inline constexpr size_t kParallelScanMinItems = 32768;
inline constexpr size_t kParallelScanMaxThreads = 8;
inline constexpr size_t kParallelScanChunk = 8192;

// Per-worker bounded collector. Keeps the `limit` smallest items according to `less`.
template <typename Item, typename Less> class TopKCollector {
public:
  TopKCollector(size_t limit, Less less) : limit_(limit), less_(less) {}

  // True when `item` would be kept if pushed (used by callers to prune expensive scoring).
  bool Accepts(const Item &item) const {
    if (items_.size() < limit_) return true;
    return !items_.empty() && less_(item, items_.front());
  }
  bool Full() const { return items_.size() >= limit_; }
  // Worst item currently retained; only valid when Full() && limit_ > 0.
  const Item &Worst() const { return items_.front(); }

  void Push(Item item) {
    if (limit_ == 0) return;
    if (items_.size() < limit_) {
      items_.push_back(std::move(item));
      std::push_heap(items_.begin(), items_.end(), less_);
    } else if (less_(item, items_.front())) {
      std::pop_heap(items_.begin(), items_.end(), less_);
      items_.back() = std::move(item);
      std::push_heap(items_.begin(), items_.end(), less_);
    }
  }

  std::vector<Item> &items() { return items_; }

private:
  size_t limit_;
  Less less_;
  std::vector<Item> items_;
};

// Scans indices [0, n). `scan(begin, end, collector)` must push the qualifying items for that
// index range into `collector`; it may run concurrently on several threads, so it must only
// read shared state. Returns the globally smallest `limit` items sorted by `less`.
// Exceptions thrown by `scan` are rethrown on the calling thread.
template <typename Item, typename Less, typename Scan>
std::vector<Item> ParallelTopK(size_t n, size_t limit, Less less, Scan scan) {
  using Collector = TopKCollector<Item, Less>;
  std::vector<Item> merged;
  if (limit == 0 || n == 0) return merged;

  size_t threads = 1;
  if (n >= kParallelScanMinItems) {
    const size_t hw = std::max<size_t>(1, std::thread::hardware_concurrency());
    threads = std::min({hw, kParallelScanMaxThreads, (n + kParallelScanChunk - 1) / kParallelScanChunk});
  }

  if (threads <= 1) {
    Collector c(limit, less);
    scan(size_t{0}, n, c);
    merged = std::move(c.items());
  } else {
    std::atomic<size_t> next{0};
    std::exception_ptr error;
    std::mutex mu;
    std::vector<Collector> collectors(threads, Collector(limit, less));
    auto worker = [&](size_t t) {
      try {
        for (;;) {
          const size_t begin = next.fetch_add(kParallelScanChunk);
          if (begin >= n) break;
          scan(begin, std::min(n, begin + kParallelScanChunk), collectors[t]);
        }
      } catch (...) {
        std::lock_guard<std::mutex> lock(mu);
        if (!error) error = std::current_exception();
        next.store(n);
      }
    };
    std::vector<std::thread> pool;
    pool.reserve(threads - 1);
    for (size_t t = 1; t < threads; ++t) pool.emplace_back(worker, t);
    worker(0);
    for (std::thread &th : pool) th.join();
    if (error) std::rethrow_exception(error);
    for (Collector &c : collectors) {
      merged.insert(merged.end(), std::make_move_iterator(c.items().begin()),
                    std::make_move_iterator(c.items().end()));
    }
  }
  std::sort(merged.begin(), merged.end(), less);
  if (merged.size() > limit) merged.resize(limit);
  return merged;
}

} // namespace rtl_trace
