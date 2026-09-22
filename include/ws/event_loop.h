#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "ws/epoller.h"
#include "ws/timer.h"

namespace ws {

class Connection;

// 一个子 Reactor：独占一个线程、一个 epoll 实例、一个最小堆定时器、一个 eventfd。
//
// 为什么需要 eventfd：主线程 accept 到新连接后，要把这条连接交给某个子 Reactor。
// 但它不能直接去调 epoll_ctl —— epoll_ctl 不是线程安全的，而且此时那个子线程
// 很可能正阻塞在 epoll_wait 上，即使塞进去了也不会立刻被处理。
// 所以走 eventfd：主线程往里面写 8 字节把子线程从 epoll_wait 唤醒，子线程醒来后
// 在**自己的线程里**执行 pending 队列中的任务（注册 fd、加定时器……），
// 这样所有涉及该 epoll 实例的操作都只发生在 loop 线程内。
class EventLoop {
 public:
  EventLoop();
  ~EventLoop();

  EventLoop(const EventLoop&) = delete;
  EventLoop& operator=(const EventLoop&) = delete;

  void loop();
  void quit();

  // 在当前线程执行 cb；若不在 loop 线程，则入队并唤醒 loop 线程。
  void runInLoop(std::function<void()> cb);
  void queueInLoop(std::function<void()> cb);

  bool isInLoopThread() const { return thread_id_ == std::this_thread::get_id(); }

  int wakeupFd() const { return wakeup_fd_; }
  Epoller& poller() { return *poller_; }
  TimerHeap& timers() { return timers_; }

  // 待触发的定时器个数。TimerHeap 本身只归 loop 线程所有，不能跨线程读；
  // 这个原子量在每轮循环末尾刷新一次，供 /stats 之类的观察者安全读取。
  // 它也是判断「定时器堆有没有随请求数膨胀」的直接指标。
  size_t pendingTimers() const { return pending_timers_.load(std::memory_order_relaxed); }

  // 连接表只在 loop 线程内被访问，所以不加锁。
  void addConnection(const std::shared_ptr<Connection>& conn);
  void removeConnection(int fd);
  std::shared_ptr<Connection> findConnection(int fd) const;
  size_t connectionCount() const { return connections_.size(); }

  // 主 Reactor 专用：把监听 socket 注册进来，就绪时回调 doAccept。
  // 子 Reactor 不设 acceptor，只跑连接 IO。
  using Acceptor = std::function<void()>;
  void setAcceptor(int listen_fd, Acceptor acceptor);

 private:
  void wakeup();
  void handleWakeup();
  void doPendingFunctors();

  int wakeup_fd_ = -1;
  int acceptor_fd_ = -1;
  Acceptor acceptor_;
  std::atomic<size_t> pending_timers_{0};
  std::unique_ptr<Epoller> poller_;
  TimerHeap timers_;

  std::atomic<bool> quit_{false};
  std::atomic<bool> looping_{false};
  std::thread::id thread_id_;

  std::mutex mutex_;
  std::vector<std::function<void()>> pending_;
  std::atomic<bool> calling_pending_{false};

  std::unordered_map<int, std::shared_ptr<Connection>> connections_;
};

}  // namespace ws
