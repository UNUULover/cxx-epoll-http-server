#pragma once

#include <cstdint>
#include <vector>
#include <sys/epoll.h>

namespace ws {

// epoll 的薄封装。只负责注册和等待，不做任何事件分发决策。
class Epoller {
 public:
  explicit Epoller(int max_events = 1024);
  ~Epoller();

  Epoller(const Epoller&) = delete;
  Epoller& operator=(const Epoller&) = delete;

  bool add(int fd, uint32_t events);
  bool mod(int fd, uint32_t events);
  bool del(int fd);

  // 阻塞至多 timeout_ms 毫秒，返回就绪事件个数；-1 表示出错。
  int wait(int timeout_ms);

  int eventCount() const { return num_events_; }
  int fdAt(int i) const { return events_[i].data.fd; }
  uint32_t eventsAt(int i) const { return events_[i].events; }

  int fd() const { return epfd_; }

 private:
  int epfd_;
  std::vector<struct epoll_event> events_;
  int num_events_ = 0;
};

}  // namespace ws
