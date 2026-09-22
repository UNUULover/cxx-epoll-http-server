#include "ws/http_parser.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace ws {

namespace {

std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

std::string trim(const std::string& s) {
  size_t b = 0;
  size_t e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

// 取出可读区里完整的一行（不含 CRLF）。返回 false 表示这一行还没收全。
bool peekLine(const Buffer& buf, std::string* line, size_t* consumed) {
  const size_t crlf = buf.findCRLF();
  if (crlf == Buffer::npos) return false;
  *line = std::string(buf.peek(), crlf);
  *consumed = crlf + 2;
  return true;
}

}  // namespace

HttpParser::Ret HttpParser::parse(Buffer* buf) {
  // 循环推进状态：请求已经完整到达时（绝大多数情况），一次调用就能走完全程。
  for (;;) {
    Ret r;
    switch (state_) {
      case State::REQUEST_LINE: r = parseRequestLine(buf); break;
      case State::HEADERS:      r = parseHeaders(buf); break;
      case State::BODY:         r = parseBody(buf); break;
      case State::COMPLETE:     return Ret::OK;
      case State::ERROR:        return Ret::ERROR;
      default:                  return Ret::ERROR;
    }
    if (r != Ret::OK) return r;  // INCOMPLETE 或 ERROR 直接冒泡给调用方
  }
}

HttpParser::Ret HttpParser::parseRequestLine(Buffer* buf) {
  std::string line;
  size_t consumed = 0;
  if (!peekLine(*buf, &line, &consumed)) {
    // 一行都没收到就超长，说明对端在灌垃圾，直接判死。
    if (buf->readableBytes() > kMaxRequestLine) {
      state_ = State::ERROR;
      return Ret::ERROR;
    }
    return Ret::INCOMPLETE;
  }
  if (line.size() > kMaxRequestLine || line.empty()) {
    state_ = State::ERROR;
    return Ret::ERROR;
  }

  const size_t sp1 = line.find(' ');
  if (sp1 == std::string::npos) {
    state_ = State::ERROR;
    return Ret::ERROR;
  }
  const size_t sp2 = line.find(' ', sp1 + 1);
  if (sp2 == std::string::npos) {
    state_ = State::ERROR;
    return Ret::ERROR;
  }

  const std::string method = line.substr(0, sp1);
  const std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
  const std::string version = line.substr(sp2 + 1);

  // 只接受 HTTP/1.x。放行 "HTTP/9.9" 这种版本号会让后面的语义判断失去意义。
  if (method.empty() || version.size() < 8 || version.compare(0, 7, "HTTP/1.") != 0) {
    state_ = State::ERROR;
    return Ret::ERROR;
  }

  request_.method = method;
  request_.version = version;

  const size_t q = target.find('?');
  if (q == std::string::npos) {
    request_.path = target;
  } else {
    request_.path = target.substr(0, q);
    request_.query = target.substr(q + 1);
  }
  if (request_.path.empty()) request_.path = "/";

  buf->retrieve(consumed);
  state_ = State::HEADERS;
  return Ret::OK;
}

HttpParser::Ret HttpParser::parseHeaders(Buffer* buf) {
  for (;;) {
    std::string line;
    size_t consumed = 0;
    if (!peekLine(*buf, &line, &consumed)) {
      if (buf->readableBytes() > kMaxRequestLine) {
        state_ = State::ERROR;
        return Ret::ERROR;
      }
      return Ret::INCOMPLETE;
    }
    buf->retrieve(consumed);

    // 空行 = 头部结束，报文头到此为止。
    if (line.empty()) {
      const std::string te = request_.getHeader("transfer-encoding");
      if (!te.empty() && !iequals(te, "identity")) {
        // 本实现只支持 Content-Length 定长 body；chunked 由上层回 501。
        state_ = State::ERROR;
        return Ret::ERROR;
      }
      if (content_length_ == 0) {
        state_ = State::COMPLETE;
        return Ret::OK;
      }
      if (content_length_ > kMaxBody) {
        state_ = State::ERROR;
        return Ret::ERROR;
      }
      state_ = State::BODY;
      return Ret::OK;
    }

    if (request_.headers.size() >= kMaxHeaderCount) {
      state_ = State::ERROR;
      return Ret::ERROR;
    }

    const size_t colon = line.find(':');
    if (colon == std::string::npos) {
      state_ = State::ERROR;
      return Ret::ERROR;
    }
    const std::string key = toLower(trim(line.substr(0, colon)));
    const std::string value = trim(line.substr(colon + 1));
    if (key.empty()) {
      state_ = State::ERROR;
      return Ret::ERROR;
    }

    if (key == "content-length") {
      char* endp = nullptr;
      const long long v = strtoll(value.c_str(), &endp, 10);
      if (endp == value.c_str() || v < 0) {
        state_ = State::ERROR;
        return Ret::ERROR;
      }
      content_length_ = static_cast<size_t>(v);
    }

    request_.headers[key] = value;
    // 继续读下一行
  }
}

HttpParser::Ret HttpParser::parseBody(Buffer* buf) {
  if (buf->readableBytes() < content_length_) return Ret::INCOMPLETE;
  request_.body = buf->retrieveAsString(content_length_);
  state_ = State::COMPLETE;
  return Ret::OK;
}

void HttpParser::reset() {
  state_ = State::REQUEST_LINE;
  request_.clear();
  content_length_ = 0;
}

}  // namespace ws
