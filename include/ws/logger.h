#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ws {

enum LogLevel { LOG_TRACE = 0, LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR, LOG_FATAL };

const char* levelName(LogLevel lv);

// 只负责把一行日志格式化成字符串，然后交给 AsyncLogger。
// 单独拆出来是为了让格式化开销留在业务线程的栈上，日志线程不做 printf。
class Logger {
 public:
  static void write(LogLevel lv, const char* file, int line, const char* fmt, ...);
};

// 异步日志：业务线程只做一次 memcpy 入队，落盘交给独立线程。
//
// 双缓冲的含义：
//   current_   —— 业务线程正在往里追加的缓冲区
//   to_write_  —— 日志线程拿到手、正在落盘的那一份
// 两者由 mutex_ 保护的 swap() 交换，因此落盘期间业务线程可以继续写 current_，
// 不会阻塞在磁盘 IO 上。
class AsyncLogger {
 public:
  static AsyncLogger& instance();

  // path 为空或 "-" 时写 stdout，便于调试。
  void start(const std::string& path, size_t flush_ms = 1000,
             size_t roll_bytes = 256u * 1024 * 1024);
  void stop();

  void setLevel(LogLevel lv) { level_.store(lv, std::memory_order_relaxed); }
  LogLevel level() const { return level_.load(std::memory_order_relaxed); }

  // 由 Logger::write 调用：线程安全，不碰磁盘。
  void append(const char* data, size_t len);

  // 给测试和压测观察用的计数。
  size_t bytesWritten() const { return bytes_written_.load(std::memory_order_relaxed); }
  size_t enqueuedBytes() const { return enqueued_bytes_.load(std::memory_order_relaxed); }

 private:
  AsyncLogger();
  ~AsyncLogger();
  AsyncLogger(const AsyncLogger&) = delete;
  AsyncLogger& operator=(const AsyncLogger&) = delete;

  void threadFunc();
  void writeAll(const char* data, size_t len);
  void rollIfNeeded(size_t incoming);

  static constexpr size_t kBufferSize = 64 * 1024;

  std::mutex mutex_;
  std::condition_variable cv_;
  std::thread thread_;

  std::vector<char> current_;    // 业务线程写入
  std::vector<char> to_write_;   // 日志线程落盘

  std::string path_;
  size_t flush_ms_ = 1000;
  size_t roll_bytes_ = 256u * 1024 * 1024;
  size_t written_this_file_ = 0;
  int fd_ = -1;

  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};
  std::atomic<LogLevel> level_{LOG_INFO};
  std::atomic<size_t> bytes_written_{0};
  std::atomic<size_t> enqueued_bytes_{0};
};

}  // namespace ws

#define WS_LOG_IMPL(lv, fmt, ...)                                              \
  do {                                                                         \
    if (::ws::AsyncLogger::instance().level() <= (lv)) {                       \
      ::ws::Logger::write((lv), __FILE__, __LINE__, fmt, ##__VA_ARGS__);       \
    }                                                                          \
  } while (0)

#define LOG_TRACE(fmt, ...) WS_LOG_IMPL(::ws::LOG_TRACE, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) WS_LOG_IMPL(::ws::LOG_DEBUG, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) WS_LOG_IMPL(::ws::LOG_INFO, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) WS_LOG_IMPL(::ws::LOG_WARN, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) WS_LOG_IMPL(::ws::LOG_ERROR, fmt, ##__VA_ARGS__)
