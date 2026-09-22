#include "ws/server.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ws/connection.h"

namespace ws {

namespace {

std::string contentTypeOf(const std::string& path) {
  const size_t dot = path.rfind('.');
  if (dot == std::string::npos) return "application/octet-stream";
  const std::string ext = path.substr(dot);
  if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
  if (ext == ".css") return "text/css; charset=utf-8";
  if (ext == ".js") return "application/javascript; charset=utf-8";
  if (ext == ".json") return "application/json; charset=utf-8";
  if (ext == ".txt") return "text/plain; charset=utf-8";
  if (ext == ".png") return "image/png";
  if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
  if (ext == ".svg") return "image/svg+xml";
  return "application/octet-stream";
}

}  // namespace

Server::Server(Options opts) : opts_(std::move(opts)), main_loop_(std::make_unique<EventLoop>()) {}

Server::~Server() { stop(); }

bool Server::initListenSocket() {
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    LOG_ERROR("socket() failed: %s", strerror(errno));
    return false;
  }

  int on = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(opts_.port);

  if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    LOG_ERROR("bind() failed on port %u: %s", opts_.port, strerror(errno));
    ::close(fd);
    return false;
  }
  if (::listen(fd, SOMAXCONN) < 0) {
    LOG_ERROR("listen() failed: %s", strerror(errno));
    ::close(fd);
    return false;
  }

  // port 传 0 时由内核分配，读回来告诉调用方，测试里要用。
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &len) == 0) {
    listen_port_ = ntohs(addr.sin_port);
  } else {
    listen_port_ = opts_.port;
  }

  listen_fd_ = fd;
  return true;
}

bool Server::start() {
  if (!initListenSocket()) return false;

  // 子 Reactor：每个线程一个 EventLoop（自己的 epoll + 定时器 + eventfd）。
  for (int i = 0; i < opts_.io_threads; ++i) {
    sub_loops_.push_back(std::make_unique<EventLoop>());
  }
  for (auto& l : sub_loops_) {
    EventLoop* loop = l.get();
    threads_.emplace_back([loop]() { loop->loop(); });
  }

  // 主 Reactor 只管 accept；io_threads == 0 时它也顺带处理连接 IO。
  main_loop_->setAcceptor(listen_fd_, [this]() { onAccept(); });

  LOG_INFO("ws-server listening on 0.0.0.0:%d (io_threads=%d, doc_root=%s, idle_timeout=%ds)",
           listen_port_, opts_.io_threads, opts_.doc_root.c_str(), opts_.idle_timeout_s);

  main_loop_->loop();  // 阻塞在这里直到 stop()

  for (auto& l : sub_loops_) l->quit();
  for (auto& t : threads_) {
    if (t.joinable()) t.join();
  }
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }

  const Stats s = snapshot();
  LOG_INFO("ws-server stopped: accepted=%llu requests=%llu responses=%llu errors=%llu",
           static_cast<unsigned long long>(s.accepted),
           static_cast<unsigned long long>(s.requests),
           static_cast<unsigned long long>(s.responses),
           static_cast<unsigned long long>(s.errors));
  return true;
}

void Server::stop() {
  if (main_loop_) main_loop_->quit();
  for (auto& l : sub_loops_) l->quit();
}

Server::Stats Server::snapshot() const {
  Stats s;
  s.accepted = counters_.accepted.load(std::memory_order_relaxed);
  s.requests = counters_.requests.load(std::memory_order_relaxed);
  s.responses = counters_.responses.load(std::memory_order_relaxed);
  s.errors = counters_.errors.load(std::memory_order_relaxed);
  s.timeouts_closed = counters_.timeouts_closed.load(std::memory_order_relaxed);
  s.bytes_read = counters_.bytes_read.load(std::memory_order_relaxed);
  s.bytes_written = counters_.bytes_written.load(std::memory_order_relaxed);
  s.active_connections = counters_.activeConnections();
  s.pending_timers = main_loop_ ? main_loop_->pendingTimers() : 0;
  for (const auto& l : sub_loops_) {
    s.pending_timers += l->pendingTimers();
  }
  return s;
}

EventLoop* Server::pickSubLoop() {
  if (sub_loops_.empty()) return main_loop_.get();
  // 轮询分发：比「谁连接少给谁」便宜，且在连接数相近时效果一样。
  const size_t idx = next_sub_.fetch_add(1, std::memory_order_relaxed) % sub_loops_.size();
  return sub_loops_[idx].get();
}

void Server::onAccept() {
  // 监听 fd 是 LT，但一次事件里可能有多个连接排着，循环 accept 到 EAGAIN 为止。
  for (;;) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    const int conn_fd = ::accept4(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), &len,
                                 SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (conn_fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      if (errno == EINTR) continue;
      if (errno == EMFILE || errno == ENFILE) {
        LOG_WARN("accept: file descriptor limit reached");
      } else {
        LOG_WARN("accept() failed: %s", strerror(errno));
      }
      break;
    }

    if (counters_.activeConnections() >= opts_.max_connections) {
      counters_.errors.fetch_add(1, std::memory_order_relaxed);
      ::close(conn_fd);
      continue;
    }

    if (opts_.tcp_nodelay) {
      int on = 1;
      ::setsockopt(conn_fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    }

    EventLoop* loop = pickSubLoop();
    // 注册 fd 这件事必须发生在**它所属的 loop 线程**里：epoll_ctl 不是线程安全的，
    // 而且目标线程可能正阻塞在 epoll_wait 上。runInLoop 会往 eventfd 写 8 字节
    // 把它唤醒，由它自己在 loop 线程里执行 attachConnection。
    loop->runInLoop([this, loop, conn_fd, addr]() { attachConnection(loop, conn_fd, addr); });
  }
}

void Server::attachConnection(EventLoop* loop, int fd, const struct sockaddr_in& addr) {
  auto conn = std::make_shared<Connection>(
      loop, fd, addr,
      [this](const HttpRequest& req, HttpResponse* resp) { handleRequest(req, resp); },
      &counters_, opts_.idle_timeout_s);

  loop->addConnection(conn);
  // 连接用 ET：配合 handleRead 里的读空循环，一个事件处理完一批数据。
  loop->poller().add(fd, EPOLLIN | EPOLLET | EPOLLRDHUP);
  // 必须在 shared_ptr 建立之后再装定时器，否则 weak_from_this() 立刻失效。
  conn->armIdleTimer();

  counters_.accepted.fetch_add(1, std::memory_order_relaxed);
  counters_.opened.fetch_add(1, std::memory_order_relaxed);
}

void Server::handleRequest(const HttpRequest& req, HttpResponse* resp) {
  const bool is_head = (req.method == "HEAD");
  const bool get_like = (req.method == "GET" || is_head);

  if (req.path == "/stats") {
    const Stats s = snapshot();
    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"accepted\":%llu,\"requests\":%llu,\"responses\":%llu,\"errors\":%llu,"
             "\"timeouts_closed\":%llu,\"bytes_read\":%llu,\"bytes_written\":%llu,"
             "\"active_connections\":%lld,\"pending_timers\":%zu}\n",
             static_cast<unsigned long long>(s.accepted),
             static_cast<unsigned long long>(s.requests),
             static_cast<unsigned long long>(s.responses),
             static_cast<unsigned long long>(s.errors),
             static_cast<unsigned long long>(s.timeouts_closed),
             static_cast<unsigned long long>(s.bytes_read),
             static_cast<unsigned long long>(s.bytes_written),
             static_cast<long long>(s.active_connections),
             s.pending_timers);
    *resp = HttpResponse::make(200, buf, "application/json; charset=utf-8");
  } else if (req.path == "/echo") {
    if (req.method == "POST" || req.method == "PUT") {
      *resp = HttpResponse::make(200, req.body, "text/plain; charset=utf-8");
    } else {
      resp->status = 405;
      *resp = HttpResponse::make(405, "405 Method Not Allowed\n");
    }
  } else if (get_like && (req.path == "/hello" || req.path == "/healthz")) {
    // 固定的小响应，不碰磁盘：压测时用它隔离出网络栈和协议解析的开销。
    *resp = HttpResponse::make(200, "Hello from ws-server\n");
  } else if (get_like) {
    serveStatic(req, resp);
  } else {
    *resp = HttpResponse::make(405, "405 Method Not Allowed\n");
  }

  resp->setHeader("Server", "ws-server/1.0");

  // HEAD 要给出和 GET 一样的头部（含 Content-Length），但不带 body。
  if (is_head) {
    resp->setHeader("Content-Length", std::to_string(resp->body.size()));
    resp->body.clear();
  }
}

void Server::serveStatic(const HttpRequest& req, HttpResponse* resp) {
  std::string rel = req.path;
  if (rel == "/") rel = "/index.html";

  // 目录穿越防护：路径里不允许出现 ..
  if (rel.find("..") != std::string::npos) {
    *resp = HttpResponse::make(403, "403 Forbidden\n");
    return;
  }

  const std::string full = opts_.doc_root + rel;

  struct stat st;
  if (::stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
    *resp = HttpResponse::make(404, "404 Not Found\n");
    return;
  }

  std::ifstream in(full, std::ios::binary);
  if (!in) {
    *resp = HttpResponse::make(404, "404 Not Found\n");
    return;
  }
  std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

  resp->status = 200;
  resp->body = std::move(data);
  resp->setHeader("Content-Type", contentTypeOf(full));
  resp->setHeader("Content-Length", std::to_string(resp->body.size()));
}

}  // namespace ws
