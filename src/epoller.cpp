#include "ws/epoller.h"

#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace ws {

Epoller::Epoller(int max_events) : epfd_(::epoll_create1(EPOLL_CLOEXEC)), events_(max_events) {}

Epoller::~Epoller() {
  if (epfd_ >= 0) ::close(epfd_);
}

bool Epoller::add(int fd, uint32_t events) {
  if (fd < 0 || epfd_ < 0) return false;
  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = events;
  ev.data.fd = fd;
  return ::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) == 0;
}

bool Epoller::mod(int fd, uint32_t events) {
  if (fd < 0 || epfd_ < 0) return false;
  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = events;
  ev.data.fd = fd;
  return ::epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) == 0;
}

bool Epoller::del(int fd) {
  if (fd < 0 || epfd_ < 0) return false;
  return ::epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr) == 0;
}

int Epoller::wait(int timeout_ms) {
  do {
    num_events_ = ::epoll_wait(epfd_, events_.data(), static_cast<int>(events_.size()), timeout_ms);
  } while (num_events_ < 0 && errno == EINTR);
  return num_events_;
}

}  // namespace ws
