#pragma once

#include <cstddef>

#include "ws/buffer.h"
#include "ws/http.h"

namespace ws {

// HTTP/1.1 请求解析器 —— 显式的有限状态机。
//
//   REQUEST_LINE ──> HEADERS ──> [BODY] ──> COMPLETE
//        │              │          │
//        └──────────────┴──────────┴──> ERROR
//
// 三个状态正好对应报文的三段。每次 parse() 都从**上次停下的状态**继续，
// 数据没到齐就返回 INCOMPLETE，已经解析掉的部分不再重复解析 —— 这正是用状态机
// 而不是「每次从头扫一遍」的理由：一个请求可能被拆成十几个 TCP 段陆续到达。
//
// 一次 parse() 内部用循环推进状态，所以「完整请求已到达」的常见情况下，
// 一次调用就能走完全程返回 OK。
class HttpParser {
 public:
  enum class Ret { OK, INCOMPLETE, ERROR };

  HttpParser() = default;

  Ret parse(Buffer* buf);

  const HttpRequest& request() const { return request_; }
  // 取走当前请求并复位，准备解析同一个连接上的下一个请求（pipelining）。
  void reset();

  // 上限，防止畸形/恶意请求把内存吃光。
  static constexpr size_t kMaxRequestLine = 8 * 1024;
  static constexpr size_t kMaxHeaderCount = 100;
  static constexpr size_t kMaxBody = 8 * 1024 * 1024;

 private:
  enum class State { REQUEST_LINE, HEADERS, BODY, COMPLETE, ERROR };

  Ret parseRequestLine(Buffer* buf);
  Ret parseHeaders(Buffer* buf);
  Ret parseBody(Buffer* buf);

  State state_ = State::REQUEST_LINE;
  HttpRequest request_;
  size_t content_length_ = 0;
};

}  // namespace ws
