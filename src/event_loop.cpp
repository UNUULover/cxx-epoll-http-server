#include "ws/event_loop.h"

#include <cerrno>
#include <cstring>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "ws/connection.h"
#include "ws/logger.h"

namespace ws {

EventLoop::EventLoop() : poller_(std::make_unique<Epoller>()) {
  // EFD_NONBLOCK：唤醒 fd 被读空后再读会返回 EAGAIN，而不会把 loop 卡住。
  wakeup_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (wakeup_fd_ < 0) {
    LOG_ERROR("eventfd() failed: %s", strerror(errno));
    abort();
  }
  poller_->add(wakeup_fd_, EPOLLIN);
}

EventLoop::~EventLoop() {
  if (wakeup_fd_ >= 0) ::close(wakeup_fd_);
}

void EventLoop::quit() {
  quit_.store(true);
  // 可能正阻塞在 epoll_wait 里，唤醒它才能看到 quit 标志。
  if (!isInLoopThread()) wakeup();
}

void EventLoop::loop() {
  looping_.store(true);
  thread_id_ = std::this_thread::get_id();

  while (!quit_.load()) {
    // 用「距下一个定时器到点还有多久」作为 epoll_wait 的超时：
    // 有定时器就精确睡到那一刻，没有就睡 200ms 兜底。
    int timeout = timers_.nextTimeoutMs();
    if (timeout < 0) timeout = 200;

    const int n = poller_->wait(timeout);
    if (n < 0) {
      LOG_ERROR("epoll_wait failed: %s", strerror(errno));
      continue;
    }

    for (int i = 0; i < n; ++i) {
      const int fd = poller_->fdAt(i);
      const uint32_t ev = poller_->eventsAt(i);

      if (fd == wakeup_fd_) {
        handleWakeup();
        continue;
      }

      // 主 Reactor：监听 socket 就绪 -> 交给 Server 去 accept。
      if (fd == acceptor_fd_) {
        if (acceptor_) acceptor_();
        continue;
      }

      auto it = connections_.find(fd);
      if (it == connections_.end()) continue;

      // 拿到 shared_ptr 再处理：回调里可能会把连接从表中摘掉（close），
      // 这里持有一份引用可以保证对象在本轮迭代结束前不被析构。
      std::shared_ptr<Connection> conn = it->second;

      if (ev & (EPOLLHUP | EPOLLERR)) {
        conn->handleError();
        continue;
      }
      if (ev & (EPOLLIN | EPOLLRDHUP)) {
        conn->handleRead();
      }
      if (conn->fd() >= 0 && (ev & EPOLLOUT)) {
        conn->handleWrite();
      }
    }

    timers_.expire();
    pending_timers_.store(timers_.size(), std::memory_order_relaxed);
    doPendingFunctors();
  }

  looping_.store(false);
}

void EventLoop::runInLoop(std::function<void()> cb) {
  if (isInLoopThread() && looping_.load()) {
    cb();  // 已经在这个线程里了，直接跑，省一次唤醒
  } else {
    queueInLoop(std::move(cb));
  }
}

void EventLoop::queueInLoop(std::function<void()> cb) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.push_back(std::move(cb));
  }
  // 如果当前就在 loop 线程里、且正在执行 pending 队列，就不必唤醒自己
  // （新任务会在本轮 doPendingFunctors 的下一轮被取走）。
  if (!isInLoopThread() || calling_pending_.load()) wakeup();
}

void EventLoop::wakeup() {
  const uint64_t one = 1;
  const ssize_t n = ::write(wakeup_fd_, &one, sizeof(one));
  (void)n;  // eventfd 计数溢出前不会失败；真失败也无非是晚一轮被处理
}

void EventLoop::handleWakeup() {
  uint64_t value = 0;
  while (::read(wakeup_fd_, &value, sizeof(value)) > 0) {
    // 读空为止，把 eventfd 计数清零
  }
}

void EventLoop::doPendingFunctors() {
  std::vector<std::function<void()>> pending;
  calling_pending_.store(true);
  {
    // 交换而不是在锁内直接执行：否则执行任务时持锁，其他线程想入队会被卡住。
    std::lock_guard<std::mutex> lock(mutex_);
    pending.swap(pending_);
  }
  for (auto& fn : pending) {
    if (fn) fn();
  }
  calling_pending_.store(false);
}

void EventLoop::addConnection(const std::shared_ptr<Connection>& conn) {
  if (!conn) return;
  connections_[conn->fd()] = conn;
}

void EventLoop::removeConnection(int fd) { connections_.erase(fd); }

std::shared_ptr<Connection> EventLoop::findConnection(int fd) const {
  auto it = connections_.find(fd);
  return it == connections_.end() ? nullptr : it->second;
}

void EventLoop::setAcceptor(int listen_fd, Acceptor acceptor) {
  acceptor_fd_ = listen_fd;
  acceptor_ = std::move(acceptor);
  // 监听 socket 用水平触发（LT）：即使一次没 accept 完，下次 epoll_wait 还会
  // 继续报可读，不会漏连接。连接的读写才用 ET。
  poller_->add(listen_fd, EPOLLIN);
}

}  // namespace ws
