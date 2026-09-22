#pragma once

#include <cstddef>
#include <string>
#include <vector>
#include <sys/types.h>

namespace ws {

// 连接上读写共用的字节缓冲区。
//
// 读缓冲：readFd() 用 readv 一次把内核数据搬进来 —— 先在栈上准备一块 64KB 的
// 临时区，这样当用户缓冲区已经够大时可以少一次系统调用。
// 写缓冲：应用层把响应 append 进来，EPOLLOUT 就绪时再吐出去。
class Buffer {
 public:
  static constexpr size_t kInitialSize = 1024;
  static constexpr size_t kPrependSize = 8;

  explicit Buffer(size_t initial_size = kInitialSize);

  size_t readableBytes() const { return write_idx_ - read_idx_; }
  size_t writableBytes() const { return buf_.size() - write_idx_; }
  size_t prependableBytes() const { return read_idx_; }

  const char* peek() const { return raw() + read_idx_; }
  char* beginWrite() { return raw() + write_idx_; }
  const char* beginWrite() const { return raw() + write_idx_; }

  // 在可读区中从 offset 处开始查找 CRLF，返回相对可读区起点的偏移。
  // 找不到返回 npos。HTTP 报文是按行解析的，所以这一步是热路径。
  static constexpr size_t npos = static_cast<size_t>(-1);
  size_t findCRLF(size_t offset = 0) const;

  void retrieve(size_t len);
  void retrieveAll();
  std::string retrieveAllAsString();
  std::string retrieveAsString(size_t len);

  void append(const char* data, size_t len);
  void append(const std::string& s) { append(s.data(), s.size()); }
  void ensureWritableBytes(size_t len);

  // 把 fd 上的数据读进缓冲区。返回读到的字节数，-1 表示出错（errno 存进 saved_errno）。
  ssize_t readFd(int fd, int* saved_errno);

  void shrink(size_t reserve);

 private:
  char* raw() { return buf_.data(); }
  const char* raw() const { return buf_.data(); }
  void makeSpace(size_t len);

  std::vector<char> buf_;
  size_t read_idx_;
  size_t write_idx_;
};

}  // namespace ws
