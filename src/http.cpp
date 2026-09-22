#include "ws/http.h"

#include <algorithm>
#include <cctype>

namespace ws {

bool iequals(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

std::string HttpRequest::getHeader(const std::string& key) const {
  auto it = headers.find(key);
  return it == headers.end() ? std::string() : it->second;
}

bool HttpRequest::hasHeader(const std::string& key) const {
  return headers.find(key) != headers.end();
}

// HTTP/1.0 默认短连接（要显式 keep-alive 才保持），
// HTTP/1.1 默认长连接（要显式 close 才断开）。
bool HttpRequest::keepAlive() const {
  const std::string conn = getHeader("connection");
  if (version == "HTTP/1.0") {
    return iequals(conn, "keep-alive");
  }
  return !iequals(conn, "close");
}

void HttpRequest::clear() {
  method.clear();
  path.clear();
  query.clear();
  version.clear();
  headers.clear();
  body.clear();
}

const char* HttpResponse::reasonPhrase(int status) {
  switch (status) {
    case 100: return "Continue";
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    default: return "Unknown";
  }
}

HttpResponse HttpResponse::make(int status, const std::string& body, const std::string& content_type) {
  HttpResponse r;
  r.status = status;
  r.body = body;
  r.headers["Content-Type"] = content_type;
  return r;
}

std::string HttpResponse::toString() const {
  std::string out;
  out.reserve(256 + body.size());

  out += "HTTP/1.1 ";
  out += std::to_string(status);
  out += ' ';
  out += reason.empty() ? reasonPhrase(status) : reason;
  out += "\r\n";

  for (const auto& kv : headers) {
    out += kv.first;
    out += ": ";
    out += kv.second;
    out += "\r\n";
  }

  // Content-Length 是 keep-alive 能工作的前提：客户端靠它知道一条响应在哪里结束。
  if (headers.find("Content-Length") == headers.end()) {
    out += "Content-Length: ";
    out += std::to_string(body.size());
    out += "\r\n";
  }

  out += "\r\n";
  out += body;
  return out;
}

}  // namespace ws
