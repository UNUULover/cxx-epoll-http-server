#include "ws/logger.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>

namespace ws {

const char* levelName(LogLevel lv) {
  switch (lv) {
    case LOG_TRACE: return "TRACE";
    case LOG_DEBUG: return "DEBUG";
    case LOG_INFO:  return "INFO";
    case LOG_WARN:  return "WARN";
    case LOG_ERROR: return "ERROR";
    case LOG_FATAL: return "FATAL";
  }
  return "?";
}

namespace {
constexpr size_t kStackBuf = 4096;

// 每个业务线程自己的格式化暂存区。日志热路径上不想为了格式化去抢锁。
thread_local char t_line[kStackBuf];
}  // namespace

void Logger::write(LogLevel lv, const char* file, int line, const char* fmt, ...) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  struct tm tm_buf;
  localtime_r(&ts.tv_sec, &tm_buf);

  char head[80];
  int hn = snprintf(head, sizeof(head), "%04d-%02d-%02d %02d:%02d:%02d.%03ld %-5s ",
                    tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                    tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                    ts.tv_nsec / 1000000, levelName(lv));
  if (hn < 0) return;
  const size_t hlen = (static_cast<size_t>(hn) < sizeof(head)) ? static_cast<size_t>(hn)
                                                              : sizeof(head) - 1;
  if (hlen == 0 || hlen >= sizeof(t_line)) return;

  // 时间戳和级别在**调用线程**上格式化好：这样日志行记录的是事件发生的时刻，
  // 而不是落盘线程稍后处理的时刻，日志线程也不必做 printf。
  memcpy(t_line, head, hlen);

  const char* slash = strrchr(file, '/');
  const char* base = slash ? slash + 1 : file;

  size_t used = hlen;
  if (used < sizeof(t_line)) {
    int n = snprintf(t_line + used, sizeof(t_line) - used, "%s:%d ", base, line);
    if (n < 0) return;
    used += std::min(static_cast<size_t>(n), sizeof(t_line) - used - 1);
  }

  va_list ap;
  va_start(ap, fmt);
  int m = vsnprintf(t_line + used, sizeof(t_line) - used, fmt, ap);
  va_end(ap);
  if (m < 0) return;
  used += std::min(static_cast<size_t>(m), sizeof(t_line) - used - 1);

  if (used + 1 < sizeof(t_line)) {
    t_line[used++] = '\n';
  }
  AsyncLogger::instance().append(t_line, used);
}

// ---------------------------------------------------------------- AsyncLogger

AsyncLogger::AsyncLogger() {
  current_.reserve(kBufferSize);
  to_write_.reserve(kBufferSize);
}

AsyncLogger::~AsyncLogger() { stop(); }

AsyncLogger& AsyncLogger::instance() {
  static AsyncLogger inst;
  return inst;
}

void AsyncLogger::start(const std::string& path, size_t flush_ms, size_t roll_bytes) {
  if (running_.load()) return;

  std::lock_guard<std::mutex> lock(mutex_);
  path_ = path;
  flush_ms_ = flush_ms;
  roll_bytes_ = roll_bytes;
  written_this_file_ = 0;
  stop_requested_.store(false);
  current_.clear();
  to_write_.clear();

  if (path_.empty() || path_ == "-") {
    fd_ = STDOUT_FILENO;
  } else {
    fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd_ < 0) fd_ = STDOUT_FILENO;  // 日志打不开也不能让服务起不来
  }

  running_.store(true);
  thread_ = std::thread(&AsyncLogger::threadFunc, this);
}

void AsyncLogger::stop() {
  if (!running_.load()) return;
  stop_requested_.store(true);
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
  running_.store(false);

  // 线程退出后再兜一次底，保证最后几行不丢。
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!current_.empty()) {
      writeAll(current_.data(), current_.size());
      current_.clear();
    }
    if (!to_write_.empty()) {
      writeAll(to_write_.data(), to_write_.size());
      to_write_.clear();
    }
  }
  if (fd_ >= 0 && fd_ != STDOUT_FILENO) {
    ::fsync(fd_);
    ::close(fd_);
  }
  fd_ = -1;
}

// 业务线程只到这一行为止：一次 memcpy 进 current_，不碰磁盘。
void AsyncLogger::append(const char* data, size_t len) {
  if (!running_.load(std::memory_order_relaxed) || len == 0) return;

  std::lock_guard<std::mutex> lock(mutex_);
  current_.insert(current_.end(), data, data + len);
  enqueued_bytes_.fetch_add(len, std::memory_order_relaxed);

  if (current_.size() >= kBufferSize) {
    cv_.notify_one();  // 攒满了，催日志线程来换手
  }
}

void AsyncLogger::threadFunc() {
  while (true) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (current_.empty() && !stop_requested_.load()) {
        cv_.wait_for(lock, std::chrono::milliseconds(flush_ms_), [this] {
          return !current_.empty() || stop_requested_.load();
        });
      }
      if (current_.empty()) {
        if (stop_requested_.load()) break;
        continue;
      }
      // 双缓冲换手：swap 之后业务线程写的是新的 current_，
      // 我们拿着 to_write_ 慢慢落盘，互不阻塞。
      to_write_.swap(current_);
    }
    writeAll(to_write_.data(), to_write_.size());
    to_write_.clear();
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (!current_.empty()) {
    writeAll(current_.data(), current_.size());
    current_.clear();
  }
}

void AsyncLogger::writeAll(const char* data, size_t len) {
  if (fd_ < 0 || len == 0) return;
  rollIfNeeded(len);

  size_t written = 0;
  while (written < len) {
    const ssize_t n = ::write(fd_, data + written, len - written);
    if (n > 0) {
      written += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    break;  // 磁盘满之类的硬错误，丢掉这一段而不是死循环
  }
  bytes_written_.fetch_add(written, std::memory_order_relaxed);
  written_this_file_ += written;
}

void AsyncLogger::rollIfNeeded(size_t incoming) {
  if (path_.empty() || path_ == "-" || fd_ < 0) return;
  if (written_this_file_ + incoming <= roll_bytes_) return;

  ::close(fd_);

  char stamp[80];
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  struct tm tm_buf;
  localtime_r(&ts.tv_sec, &tm_buf);
  snprintf(stamp, sizeof(stamp), ".%04d%02d%02d-%02d%02d%02d",
           tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
           tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);

  const std::string rotated = path_ + stamp;
  ::rename(path_.c_str(), rotated.c_str());

  fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd_ < 0) fd_ = STDOUT_FILENO;
  written_this_file_ = 0;
}

}  // namespace ws
