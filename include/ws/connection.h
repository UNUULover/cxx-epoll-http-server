#pragma once

#include <functional>
#include <memory>
#include <netinet/in.h>
#include <string>

#include "ws/buffer.h"
#include "ws/http.h"
#include "ws/http_parser.h"
#include "ws/timer.h"

namespace ws {

class EventLoop;
struct ServerCounters;

// 一条 TCP 连接。
//
// 线程归属：一条连接从建立到销毁，只会被**它所属的那个 EventLoop 线程**碰，
// 所以这里的成员一律不加锁 —— 加锁反而是错的（会掩盖真正的跨线程访问 bug）。
//
// 空闲超时的设计（面试常问）：
//   不要「每次收到数据就 cancel 掉旧定时器、再装一个新的」。那样在 1 万 QPS 下，
//   定时器堆会以每秒 1 万个的速度堆积废弃条目（取消是懒删除，要等到期才被弹掉）。
//   这里改成：每个连接**同时只有一个**待触发的定时器；到期时先看 last_activity_，
//   真的空闲才关连接，否则重新排一次。请求路径上只有一个时间戳赋值。
class Connection : public std::enable_shared_from_this<Connection> {
 public:
  using HttpHandler = std::function<void(const HttpRequest&, HttpResponse*)>;

  Connection(EventLoop* loop, int fd, const struct sockaddr_in& peer, HttpHandler handler,
             ServerCounters* counters, int idle_timeout_s);
  ~Connection();

  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  int fd() const { return fd_; }
  std::string peerIp() const { return peer_ip_; }
  uint16_t peerPort() const { return peer_port_; }

  // 只在所属 loop 线程内调用。
  void handleRead();
  void handleWrite();
  void handleError();

  void send(const std::string& data);
  void close();

  // 连接挂进 loop 之后调用一次，装上空闲检查定时器。
  // 必须在对象已经被 shared_ptr 持有之后调用，否则 weak_from_this() 立刻失效。
  void armIdleTimer();

  // 收到数据时调用：只更新一个时间戳，不做任何定时器操作。
  void touch() { last_activity_ = now(); }

 private:
  void handleRequest();
  void enableWriting(bool on);

  EventLoop* loop_;
  int fd_;
  std::string peer_ip_;
  uint16_t peer_port_ = 0;

  Buffer input_;
  Buffer output_;
  HttpParser parser_;
  HttpHandler handler_;
  ServerCounters* counters_;

  int idle_timeout_s_;
  TimerId idle_timer_ = 0;
  TimePoint last_activity_;

  bool closed_ = false;
  bool writing_ = false;
  bool close_after_write_ = false;
  bool keep_alive_ = true;
};

}  // namespace ws
