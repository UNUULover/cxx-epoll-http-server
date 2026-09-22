#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <thread>
#include <vector>

#include "ws/event_loop.h"
#include "ws/http.h"
#include "ws/logger.h"
#include "ws/stats.h"

namespace ws {

// 主从 Reactor 结构的 HTTP 服务器。
//
//   io_threads == 0   -> 单 Reactor：主线程既 accept 也处理连接 IO
//   io_threads == N   -> 主从 Reactor：主线程只 accept + 分发，
//                        N 个子 Reactor 线程各自 epoll 处理连接 IO
//
// 这两种模式共用同一份代码，只差一个参数 —— 这样「架构对比」实验里的
// 自变量是干净的，压出来的差异确实来自架构本身。
class Server {
 public:
  struct Options {
    uint16_t port = 8080;
    int io_threads = 0;
    std::string doc_root = "./www";
    int idle_timeout_s = 60;
    int max_connections = 20000;
    bool tcp_nodelay = true;
  };

  struct Stats {
    uint64_t accepted = 0;
    uint64_t requests = 0;
    uint64_t responses = 0;
    uint64_t errors = 0;
    uint64_t timeouts_closed = 0;
    uint64_t bytes_read = 0;
    uint64_t bytes_written = 0;
    int64_t active_connections = 0;
    size_t pending_timers = 0;
  };

  explicit Server(Options opts);
  ~Server();

  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // 阻塞运行到 stop() 被调用。
  bool start();
  void stop();

  Stats snapshot() const;
  int listenPort() const { return listen_port_; }

 private:
  bool initListenSocket();
  void onAccept();
  void attachConnection(EventLoop* loop, int fd, const struct sockaddr_in& addr);
  EventLoop* pickSubLoop();

  void handleRequest(const HttpRequest& req, HttpResponse* resp);
  void serveStatic(const HttpRequest& req, HttpResponse* resp);

  Options opts_;
  int listen_fd_ = -1;
  int listen_port_ = 0;

  std::unique_ptr<EventLoop> main_loop_;
  std::vector<std::unique_ptr<EventLoop>> sub_loops_;
  std::vector<std::thread> threads_;
  std::atomic<size_t> next_sub_{0};

  ServerCounters counters_;
};

}  // namespace ws
