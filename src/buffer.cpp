#include "ws/buffer.h"

#include <algorithm>
#include <cstring>
#include <errno.h>
#include <sys/uio.h>
#include <unistd.h>

namespace ws {

namespace {
const char kCRLF[2] = {'\r', '\n'};
}  // namespace

Buffer::Buffer(size_t initial_size) : buf_(initial_size), read_idx_(0), write_idx_(0) {}

size_t Buffer::findCRLF(size_t offset) const {
  const size_t n = readableBytes();
  if (offset >= n) return npos;
  const char* first = peek() + offset;
  const char* last = peek() + n;
  const char* p = std::search(first, last, kCRLF, kCRLF + 2);
  if (p == last) return npos;
  return static_cast<size_t>(p - peek());
}

void Buffer::retrieve(size_t len) {
  if (len >= readableBytes()) {
    retrieveAll();
    return;
  }
  read_idx_ += len;
}

void Buffer::retrieveAll() {
  read_idx_ = 0;
  write_idx_ = 0;
}

std::string Buffer::retrieveAllAsString() { return retrieveAsString(readableBytes()); }

std::string Buffer::retrieveAsString(size_t len) {
  const size_t n = std::min(len, readableBytes());
  std::string result(peek(), n);
  retrieve(n);
  return result;
}

void Buffer::ensureWritableBytes(size_t len) {
  if (writableBytes() < len) makeSpace(len);
}

void Buffer::append(const char* data, size_t len) {
  ensureWritableBytes(len);
  std::copy(data, data + len, beginWrite());
  write_idx_ += len;
}

// 经典的空间回收：如果「已读废弃区 + 尾部空闲区」加起来够用，就把未读数据搬到
// 缓冲区头部复用，避免扩容；否则才真正 resize。
void Buffer::makeSpace(size_t len) {
  if (writableBytes() + prependableBytes() < len + kPrependSize) {
    buf_.resize(write_idx_ + len);
  } else {
    const size_t readable = readableBytes();
    std::copy(raw() + read_idx_, raw() + write_idx_, raw() + kPrependSize);
    read_idx_ = kPrependSize;
    write_idx_ = read_idx_ + readable;
  }
}

// readv：如果用户缓冲区尾巴够大就只用 1 个 iovec，否则挂上栈上的 64KB 临时区，
// 一次系统调用把数据全部读进来，少一次 epoll 循环。
ssize_t Buffer::readFd(int fd, int* saved_errno) {
  char extrabuf[65536];
  const size_t writable = writableBytes();

  struct iovec vec[2];
  vec[0].iov_base = raw() + write_idx_;
  vec[0].iov_len = writable;
  vec[1].iov_base = extrabuf;
  vec[1].iov_len = sizeof(extrabuf);

  const int iovcnt = (writable < sizeof(extrabuf)) ? 2 : 1;
  const ssize_t n = ::readv(fd, vec, iovcnt);

  if (n < 0) {
    *saved_errno = errno;
  } else if (static_cast<size_t>(n) <= writable) {
    write_idx_ += static_cast<size_t>(n);
  } else {
    write_idx_ = buf_.size();
    append(extrabuf, static_cast<size_t>(n) - writable);
  }
  return n;
}

void Buffer::shrink(size_t reserve) {
  buf_.resize(write_idx_ + reserve);
  buf_.shrink_to_fit();
}

}  // namespace ws
