#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <list>
#include <set>
#include <vector>

#include "ws/memory_pool.h"

using namespace ws;

TEST(MemoryPool, ReusesFreedBlocks) {
  MemoryPool pool(64, 4);
  void* a = pool.allocate();
  pool.deallocate(a);
  void* b = pool.allocate();
  // 空闲链表是 LIFO，刚还回来的块会被下一个请求立刻复用。
  EXPECT_EQ(a, b);
}

TEST(MemoryPool, GrowsInChunks) {
  MemoryPool pool(64, 4);
  EXPECT_EQ(pool.chunks(), 1u);  // 构造时先备好一块

  std::vector<void*> blocks;
  for (int i = 0; i < 9; ++i) blocks.push_back(pool.allocate());

  EXPECT_EQ(pool.chunks(), 3u);  // 4 + 4 + 1
  EXPECT_EQ(pool.capacity(), 12u);
  EXPECT_EQ(pool.blocksInUse(), 9u);
}

TEST(MemoryPool, BlocksAreAlignedAndDistinct) {
  MemoryPool pool(64, 16);
  std::set<void*> seen;
  for (int i = 0; i < 16; ++i) {
    void* p = pool.allocate();
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 16, 0u) << "block not 16-byte aligned";
    EXPECT_TRUE(seen.insert(p).second) << "same block handed out twice";
  }
}

TEST(MemoryPool, BlocksDoNotOverlap) {
  MemoryPool pool(64, 8);
  std::vector<uintptr_t> addrs;
  for (int i = 0; i < 8; ++i) {
    addrs.push_back(reinterpret_cast<uintptr_t>(pool.allocate()));
  }
  std::sort(addrs.begin(), addrs.end());
  for (size_t i = 1; i < addrs.size(); ++i) {
    EXPECT_GE(addrs[i] - addrs[i - 1], pool.blockSize());
  }
}

// 内存池存在的理由：N 次分配只对应很少的几次「向系统要内存」。
// 这条断言给出的就是简历上那个可比复现的数字。
TEST(MemoryPool, ManyAllocationsNeedFewChunks) {
  MemoryPool pool(128, 512);

  std::vector<void*> v;
  for (int i = 0; i < 10000; ++i) v.push_back(pool.allocate());

  EXPECT_EQ(pool.chunks(), 20u);  // ceil(10000 / 512) = 20 次系统分配，而不是 1 万次
  EXPECT_EQ(pool.capacity(), 20u * 512u);

  for (void* p : v) pool.deallocate(p);
  EXPECT_EQ(pool.blocksInUse(), 0u);
}

TEST(MemoryPool, InUseAccounting) {
  MemoryPool pool(32, 8);
  void* a = pool.allocate();
  void* b = pool.allocate();
  EXPECT_EQ(pool.blocksInUse(), 2u);

  pool.deallocate(a);
  EXPECT_EQ(pool.blocksInUse(), 1u);

  pool.deallocate(b);
  EXPECT_EQ(pool.blocksInUse(), 0u);

  pool.deallocate(nullptr);  // 空指针必须被忽略
  EXPECT_EQ(pool.blocksInUse(), 0u);
}

// 小于一个指针的块会被抬到最小尺寸，否则空闲链表没地方存 next。
TEST(MemoryPool, TinyRequestIsRaised) {
  MemoryPool pool(1, 4);
  EXPECT_GE(pool.blockSize(), sizeof(void*));
  EXPECT_NE(pool.allocate(), nullptr);
}

TEST(MemoryPool, ThreadLocalPoolIsStablePerSizeClass) {
  MemoryPool& a = tlsPoolFor(64);
  MemoryPool& b = tlsPoolFor(64);
  EXPECT_EQ(&a, &b) << "same size class must map to the same thread-local pool";

  MemoryPool& c = tlsPoolFor(128);
  EXPECT_NE(&a, &c) << "different size classes must not share a pool";
}

TEST(MemoryPool, PoolAllocatorBacksStlContainers) {
  std::list<int, PoolAllocator<int>> lst;
  for (int i = 0; i < 1000; ++i) lst.push_back(i);
  EXPECT_EQ(lst.size(), 1000u);
  EXPECT_EQ(lst.front(), 0);
  EXPECT_EQ(lst.back(), 999);

  while (!lst.empty()) lst.pop_front();
  EXPECT_TRUE(lst.empty());
}
