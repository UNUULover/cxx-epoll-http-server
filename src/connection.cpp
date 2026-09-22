#include "ws/connection.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ws/event_loop.h"
#include "ws/logger.h"
#include "ws/stats.h"

namespace ws {

namespace {

std::string ipToString(const struct sockaddr_in& addr) {
  char buf[INET_ADDRSTRLEN] = {0};
  if (!::inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf))) return "?";
  return std::string(buf);
}

// 连接上关注的事件：可读 + 边缘触发 + 对端关闭通知。
constexpr uint32_t kConnBaseEvents = EPOLLIN | EPOLLET | EPOLLRDHUP;

}  // namespace

Connection::Connection(EventLoop* loop, int fd, const struct sockaddr_in& peer,
                       HttpHandler handler, ServerCounters* counters, int idle_timeout_s)
    : loop_(loop),
      fd_(fd),
      peer_ip_(ipToString(peer)),
      peer_port_(ntohs(peer.sin_port)),
      handler_(std::move(handler)),
      counters_(counters),
      idle_timeout_s_(idle_timeout_s),
      last_activity_(now()) {
  input_.ensureWritableBytes(2048);
}

Connection::~Connection() {
  // 正常路径上 close() 已经把 fd 收掉了；这里兜住「没走 close 就被析构」的情况。
  if (fd_ >= 0) {
    loop_->poller().del(fd_);
    ::close(fd_);
    fd_ = -1;
  }
}

void Connection::close() {
  if (closed_) return;
  closed_ = true;

  const int fd = fd_;
  if (idle_timer_ != 0) {
    loop_->timers().cancel(idle_timer_);
    idle_timer_ = 0;
  }
  if (fd >= 0) {
    loop_->poller().del(fd);
    ::close(fd);
  }
  fd_ = -1;

  // 从 loop 的连接表里摘掉。这一步可能释放最后一个 shared_ptr，但调用方
  // （EventLoop::loop 或 runInLoop 的闭包）手上还持有一份引用，
  // 所以本对象在本次调用返回前不会被析构。
  loop_->removeConnection(fd);
  counters_->closed.fetch_add(1, std::memory_order_relaxed);
}

// 装定时器，但**不在每次收数据时重装**。
//
// 定时器到期时先检查 last_activity_：
//   - 确实空闲了 -> 关连接
//   - 期间有活动 -> 直接再排一次（不 cancel：旧条目已经被弹出，没什么可取消的）
// 这样每个连接同时最多只有一个待触发条目，定时器堆的大小只跟「连接数」有关，
// 与请求数无关。
void Connection::armIdleTimer() {
  if (closed_ || idle_timeout_s_ <= 0) return;

  std::weak_ptr<Connection> weak = weak_from_this();
  if (weak.expired()) return;  // 还没被 shared_ptr 接管，装了也没用

  ServerCounters* counters = counters_;
  idle_timer_ = loop_->timers().addAfter(
      static_cast<int64_t>(idle_timeout_s_) * 1000, [weak, counters]() {
        auto conn = weak.lock();
        if (!conn) return;  // 连接已经关了

        const auto idle = Clock::now() - conn->last_activity_;
        if (idle >= std::chrono::seconds(conn->idle_timeout_s_)) {
          counters->timeouts_closed.fetch_add(1, std::memory_order_relaxed);
          conn->idle_timer_ = 0;  // 本条目已经消费掉了，close() 不必再去取消
          conn->close();
        } else {
          conn->armIdleTimer();
        }
      });
}

void Connection::handleRead() {
  // ET 模式下必须一次把内核缓冲读空（读到 EAGAIN），否则不会再收到可读通知。
  for (;;) {
    int saved = 0;
    const ssize_t n = input_.readFd(fd_, &saved);
    if (n > 0) {
      counters_->bytes_read.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
      continue;
    }
    if (n == 0) {  // 对端正常关闭
      close();
      return;
    }
    if (saved == EAGAIN || saved == EWOULDBLOCK) break;
    if (saved == EINTR) continue;
    handleError();
    return;
  }

  touch();  // 有数据进来 = 连接还活着。只写时间戳，不碰定时器。
  handleRequest();
}

void Connection::handleRequest() {
  for (;;) {
    const HttpParser::Ret ret = parser_.parse(&input_);

    if (ret == HttpParser::Ret::INCOMPLETE) return;

    if (ret == HttpParser::Ret::ERROR) {
      counters_->errors.fetch_add(1, std::memory_order_relaxed);
      close_after_write_ = true;
      keep_alive_ = false;
      HttpResponse resp = HttpResponse::make(400, "400 Bad Request\n");
      resp.headers["Connection"] = "close";
      send(resp.toString());
      return;
    }

    counters_->requests.fetch_add(1, std::memory_order_relaxed);

    HttpResponse resp;
    handler_(parser_.request(), &resp);

    keep_alive_ = parser_.request().keepAlive() && !resp.close_connection;
    if (!keep_alive_) {
      resp.headers["Connection"] = "close";
      close_after_write_ = true;
    }

    send(resp.toString());
    counters_->responses.fetch_add(1, std::memory_order_relaxed);
    parser_.reset();

    if (!keep_alive_) return;
    // pipelining：一个 TCP 段里可能塞了多个请求，缓冲区还有就继续解析。
    if (input_.readableBytes() == 0) return;
  }
}

void Connection::send(const std::string& data) {
  if (closed_ || data.empty()) return;
  output_.append(data);
  // 先直接试写一次：绝大多数响应能一次写完，这样整个请求处理过程
  // 一次 epoll_ctl 都不需要（不用挂 EPOLLOUT）。
  handleWrite();
}

void Connection::handleWrite() {
  if (closed_) return;

  if (output_.readableBytes() > 0) {
    const ssize_t n = ::write(fd_, output_.peek(), output_.readableBytes());
    if (n > 0) {
      output_.retrieve(static_cast<size_t>(n));
      counters_->bytes_written.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
    } else if (n < 0) {
      if (errno == EINTR) return;  // 下轮事件再说
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        handleError();
        return;
      }
      // EAGAIN：内核发送缓冲满了，下面挂 EPOLLOUT 等它腾出空间
    }
  }

  if (output_.readableBytes() == 0) {
    if (writing_) enableWriting(false);
    if (close_after_write_) close();
  } else if (!writing_) {
    enableWriting(true);
  }
}

void Connection::enableWriting(bool on) {
  if (closed_) return;
  writing_ = on;
  uint32_t events = kConnBaseEvents;
  if (on) events |= EPOLLOUT;
  loop_->poller().mod(fd_, events);
}

void Connection::handleError() {
  counters_->errors.fetch_add(1, std::memory_order_relaxed);
  close();
}

}  // namespace ws
