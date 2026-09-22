#include "ws/memory_pool.h"

#include <cstdlib>

namespace ws {

namespace {

constexpr size_t kAlign = 16;

// 每个 block 至少要放得下一个指针（空闲链表要用它存 next），并对齐到 16 字节。
size_t normalizeBlockSize(size_t s) {
  if (s < sizeof(void*)) s = sizeof(void*);
  if (s < kAlign) s = kAlign;
  return (s + kAlign - 1) & ~(kAlign - 1);
}

// 覆盖小对象热区的 size class 表。超过 4096 字节的走 fallback 池。
constexpr size_t kClasses[] = {16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
constexpr size_t kNumClasses = sizeof(kClasses) / sizeof(kClasses[0]);

struct ClassPools {
  std::unique_ptr<MemoryPool> pools[kNumClasses];
};

}  // namespace

MemoryPool::MemoryPool(size_t block_size, size_t blocks_per_chunk)
    : block_size_(normalizeBlockSize(block_size)),
      blocks_per_chunk_(blocks_per_chunk == 0 ? 1 : blocks_per_chunk) {
  grow();  // 先备好一块，热路径上不要出现「第一次分配就扩容」
}

MemoryPool::~MemoryPool() {
  for (auto& c : chunks_) ::free(c.base);
}

void MemoryPool::grow() {
  const size_t bytes = block_size_ * blocks_per_chunk_;
  const size_t aligned = (bytes + kAlign - 1) & ~(kAlign - 1);

  void* mem = ::aligned_alloc(kAlign, aligned);
  if (!mem) throw std::bad_alloc();

  chunks_.push_back(Chunk{static_cast<char*>(mem), aligned});
  capacity_ += blocks_per_chunk_;

  // 把新 chunk 切块串进空闲链表。注意这个循环只在扩容时跑，不在分配路径上。
  char* p = static_cast<char*>(mem);
  for (size_t i = 0; i < blocks_per_chunk_; ++i) {
    void* block = p + i * block_size_;
    *static_cast<void**>(block) = free_list_;
    free_list_ = block;
  }
}

void* MemoryPool::allocate() {
  if (!free_list_) grow();
  void* p = free_list_;
  free_list_ = *static_cast<void**>(p);
  ++in_use_;
  return p;
}

void MemoryPool::deallocate(void* p) {
  if (!p) return;
  *static_cast<void**>(p) = free_list_;
  free_list_ = p;
  --in_use_;
}

MemoryPool& tlsPoolFor(size_t block_size) {
  static thread_local ClassPools t_pools;
  static thread_local std::vector<std::unique_ptr<MemoryPool>> t_big_pools;

  for (size_t i = 0; i < kNumClasses; ++i) {
    if (block_size <= kClasses[i]) {
      if (!t_pools.pools[i]) {
        t_pools.pools[i] = std::make_unique<MemoryPool>(kClasses[i], 512);
      }
      return *t_pools.pools[i];
    }
  }

  // 超大块：每个尺寸单独一个池，数量很少，线性找一下就够了。
  const size_t rounded = normalizeBlockSize(block_size);
  for (auto& p : t_big_pools) {
    if (p->blockSize() == rounded) return *p;
  }
  t_big_pools.push_back(std::make_unique<MemoryPool>(rounded, 16));
  return *t_big_pools.back();
}

}  // namespace ws
