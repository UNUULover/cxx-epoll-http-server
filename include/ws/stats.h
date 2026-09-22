#pragma once

#include <atomic>
#include <cstdint>

namespace ws {

// 全局计数器。Connection 在热路径上直接对它们做 relaxed 自增，
// 只提供最终一致读数，不参与任何控制流判断。
struct ServerCounters {
  std::atomic<uint64_t> accepted{0};
  std::atomic<uint64_t> opened{0};
  std::atomic<uint64_t> closed{0};
  std::atomic<uint64_t> requests{0};
  std::atomic<uint64_t> responses{0};
  std::atomic<uint64_t> errors{0};
  std::atomic<uint64_t> timeouts_closed{0};
  std::atomic<uint64_t> bytes_read{0};
  std::atomic<uint64_t> bytes_written{0};

  int64_t activeConnections() const {
    return static_cast<int64_t>(opened.load(std::memory_order_relaxed)) -
           static_cast<int64_t>(closed.load(std::memory_order_relaxed));
  }
};

}  // namespace ws
