#pragma once

#include <map>
#include <string>

namespace ws {

struct HttpRequest {
  std::string method;   // GET / POST / HEAD ...
  std::string path;     // 不含 query，如 /index.html
  std::string query;    // ? 之后的部分，没有则为空
  std::string version;  // HTTP/1.1
  std::map<std::string, std::string> headers;  // key 统一小写，value 已 trim
  std::string body;

  // key 需已经是小写。
  std::string getHeader(const std::string& key) const;
  bool hasHeader(const std::string& key) const;
  bool keepAlive() const;
  void clear();
};

struct HttpResponse {
  int status = 200;
  std::string reason;  // 留空则用状态码默认短语
  std::map<std::string, std::string> headers;
  std::string body;
  bool close_connection = false;

  HttpResponse() = default;

  static HttpResponse make(int status, const std::string& body,
                           const std::string& content_type = "text/plain; charset=utf-8");

  void setHeader(const std::string& key, const std::string& value) { headers[key] = value; }

  // 序列化成完整的 HTTP/1.1 报文。缺少 Content-Length 时按 body.size() 补上。
  std::string toString() const;

  static const char* reasonPhrase(int status);
};

// 大小写不敏感比较，处理 Connection: Keep-Alive 这种大小写混写。
bool iequals(const std::string& a, const std::string& b);

}  // namespace ws
