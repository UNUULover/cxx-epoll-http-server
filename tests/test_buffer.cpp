#include <gtest/gtest.h>

#include <string>

#include "ws/buffer.h"

using namespace ws;

namespace {
// 不变式：缓冲区总容量 == 已读废弃区 + 未读区 + 尾部空闲区。
size_t capacityOf(const Buffer& b) {
  return b.prependableBytes() + b.readableBytes() + b.writableBytes();
}
}  // namespace

TEST(Buffer, AppendAndRetrieve) {
  Buffer b(16);
  b.append("hello", 5);
  EXPECT_EQ(b.readableBytes(), 5u);
  EXPECT_EQ(std::string(b.peek(), 5), "hello");

  b.retrieve(2);
  EXPECT_EQ(b.readableBytes(), 3u);
  EXPECT_EQ(std::string(b.peek(), 3), "llo");
}

// retrieve 超出可读长度时应当等同于清空，而不是把索引搞成负数。
TEST(Buffer, RetrieveBeyondReadableClears) {
  Buffer b;
  b.append("abc", 3);
  b.retrieve(100);
  EXPECT_EQ(b.readableBytes(), 0u);
  EXPECT_EQ(b.writableBytes(), capacityOf(b));
}

TEST(Buffer, GrowsWithoutLosingData) {
  Buffer b(8);
  const std::string big(5000, 'x');
  b.append(big.data(), big.size());
  ASSERT_EQ(b.readableBytes(), big.size());
  EXPECT_EQ(std::string(b.peek(), big.size()), big);
}

// 空间回收：读掉一部分之后，剩余数据应当被搬回缓冲区头部复用，
// 而不是让缓冲区无限向后增长（这里用总容量不变来验证没有发生扩容）。
TEST(Buffer, ReclaimsPrependSpaceWithoutGrowing) {
  Buffer b(64);
  const size_t initial_capacity = capacityOf(b);
  ASSERT_EQ(initial_capacity, 64u);

  b.append(std::string(40, 'a'));
  b.retrieve(40);
  b.append(std::string(40, 'b'));

  EXPECT_EQ(b.readableBytes(), 40u);
  EXPECT_EQ(capacityOf(b), initial_capacity);  // 没有扩容
  EXPECT_EQ(std::string(b.peek(), 4), "bbbb");
}

TEST(Buffer, FindCRLF) {
  const std::string raw = "GET / HTTP/1.1\r\nHost: x\r\n";
  Buffer b;
  b.append(raw.data(), raw.size());

  const size_t first = raw.find("\r\n");
  const size_t second = raw.find("\r\n", first + 2);

  EXPECT_EQ(b.findCRLF(), first);
  EXPECT_EQ(b.findCRLF(first + 2), second);
  EXPECT_EQ(b.findCRLF(b.readableBytes()), Buffer::npos);
  EXPECT_EQ(b.findCRLF(b.readableBytes() + 10), Buffer::npos);
}

TEST(Buffer, RetrieveAsStringConsumes) {
  Buffer b;
  b.append(std::string("abcdef"));
  EXPECT_EQ(b.retrieveAsString(3), "abc");
  EXPECT_EQ(b.readableBytes(), 3u);
  EXPECT_EQ(b.retrieveAllAsString(), "def");
  EXPECT_EQ(b.readableBytes(), 0u);
}
