#pragma once
// Thread-safe collector for asynchronous completions. Never sleeps; every wait has a timeout so a
// hang fails the test with a message instead of stalling CTest.
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stills/generator.hpp>
#include <string>
#include <thread>
#include <vector>

#include "support/frame_index.hpp"

namespace testsupport {

struct Collected {
  std::size_t position;  // item index within the batch
  /// Completion::index again, under the name the per-item assertions below use.
  std::size_t sequence;
  stills::Time requested;
  stills::GenerationStatus status;
  std::optional<int> index;  // frame index when an image came back
  std::optional<stills::Time> actual;
  std::optional<stills::ErrorCode> error;
  std::thread::id thread;

  /// The item with this sequence number, or null.
  [[nodiscard]] static const Collected* by_sequence(const std::vector<Collected>& items,
                                                    std::size_t seq) {
    for (const auto& i : items) {
      if (i.sequence == seq) return &i;
    }
    return nullptr;
  }
};

class Collector {
 public:
  /// Handler to pass to generate_images(). Captures `this`; the Collector must outlive the batch.
  auto handler(int rotation = 0) {
    return [this, rotation](stills::Completion c) { add(std::move(c), rotation); };
  }

  void add(stills::Completion c, int rotation = 0) {
    Collected item{c.index,      c.index,      c.requested_time, c.status(),
                   std::nullopt, std::nullopt, std::nullopt,     std::this_thread::get_id()};
    if (c.result) {
      item.index = frame_index_of(*c.result, rotation);
      item.actual = c.result->actual_time();
    } else {
      item.error = c.result.error().code;
    }
    // Notify while holding the lock: a waiter may destroy this Collector as soon as it wakes.
    std::lock_guard lk(mutex_);
    items_.push_back(std::move(item));
    cv_.notify_all();
  }

  [[nodiscard]] bool wait_for_count(std::size_t n,
                                    std::chrono::seconds timeout = std::chrono::seconds{30}) {
    std::unique_lock lk(mutex_);
    return cv_.wait_for(lk, timeout, [&] { return items_.size() >= n; });
  }

  /// Blocks until `pred(items)` holds or the timeout elapses; false on the timeout, so a test that
  /// is never satisfied fails with its own message instead of stalling CTest. `pred` runs under
  /// the collector's lock, so it must not call back into the collector.
  template <class Pred>
  [[nodiscard]] bool wait_until(Pred pred,
                                std::chrono::seconds timeout = std::chrono::seconds{30}) {
    std::unique_lock lk(mutex_);
    const std::vector<Collected>& items = items_;
    return cv_.wait_for(lk, timeout, [&] { return pred(items); });
  }

  [[nodiscard]] std::vector<Collected> items() const {
    std::lock_guard lk(mutex_);
    return items_;
  }
  [[nodiscard]] std::size_t count() const {
    std::lock_guard lk(mutex_);
    return items_.size();
  }
  [[nodiscard]] std::size_t count(stills::GenerationStatus s) const {
    std::lock_guard lk(mutex_);
    std::size_t n = 0;
    for (const auto& i : items_) n += i.status == s ? 1 : 0;
    return n;
  }

  /// Every succeeded item returned the frame its requested time asks for, on a 30 fps fixture.
  /// Counting completions proves the *accounting*; this proves the *frames*, which is what the
  /// library is for — a batch that delivered the neighbouring frame every time would otherwise
  /// pass every assertion in these tests.
  [[nodiscard]] bool all_exact() const { return first_wrong().empty(); }

  /// The first item whose frame is not the one its requested time asks for, as a message; empty
  /// when they all are. Items that did not succeed are not the subject and are skipped.
  [[nodiscard]] std::string first_wrong() const {
    std::lock_guard lk(mutex_);
    for (const auto& i : items_) {
      if (i.status != stills::GenerationStatus::succeeded || !i.index) continue;
      const int want = frame_index_of_time(i.requested);
      if (*i.index != want) {
        return "item " + std::to_string(i.position) + " requested " +
               stills::to_string(i.requested) + " got frame " + std::to_string(*i.index) +
               ", expected " + std::to_string(want);
      }
    }
    return {};
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<Collected> items_;
};

}  // namespace testsupport
