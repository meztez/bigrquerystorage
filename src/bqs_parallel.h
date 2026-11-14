#ifndef BQS_PARALLEL_H
#define BQS_PARALLEL_H

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <thread>
#include <type_traits>
#include <vector>

inline int bqs_resolve_threads(int n_threads, unsigned int cap = 16u) {
  if (n_threads > 0) return n_threads;
  unsigned int hw = std::thread::hardware_concurrency();
  return hw > 0 ? static_cast<int>(std::min(hw, cap)) : 4;
}

template <typename WorkerFn>
inline void bqs_parallel_for(size_t n_tasks, int n_threads, WorkerFn&& fn) {
  if (n_tasks == 0) return;
  constexpr bool kReturnsBool =
    std::is_same_v<decltype(fn(0, static_cast<size_t>(0))), bool>;
  int n_workers = static_cast<int>(
    std::min<size_t>(static_cast<size_t>(std::max(1, n_threads)), n_tasks));
  if (n_workers <= 1) {
    for (size_t idx = 0; idx < n_tasks; idx++) {
      if constexpr (kReturnsBool) {
        if (!fn(0, idx)) break;
      } else {
        fn(0, idx);
      }
    }
    return;
  }

  std::atomic<size_t> next_idx{0};
  std::atomic<bool> stop_flag{false};
  auto loop = [&](int worker_id) {
    while (!stop_flag.load(std::memory_order_relaxed)) {
      size_t idx = next_idx.fetch_add(1, std::memory_order_relaxed);
      if (idx >= n_tasks) break;
      if constexpr (kReturnsBool) {
        if (!fn(worker_id, idx)) {
          stop_flag.store(true, std::memory_order_relaxed);
          break;
        }
      } else {
        fn(worker_id, idx);
      }
    }
  };

  std::vector<std::thread> pool;
  pool.reserve(static_cast<size_t>(n_workers - 1));
  for (int w = 1; w < n_workers; w++) {
    pool.emplace_back(loop, w);
  }
  loop(0);
  for (auto& th : pool) {
    if (th.joinable()) th.join();
  }
}

#endif // BQS_PARALLEL_H
