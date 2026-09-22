#pragma once

#include <cstddef>
#include <memory>
#include <new>
#include <vector>

namespace ws {

// 定长块内存池。
//
// 解决的问题：连接对象、缓冲区节点、HTTP 头部这些都是一次请求里反复分配释放的
// 小对象，走 glibc malloc 每次都要加锁、查 bin、可能触发 brk/mmap。
// 这里一次向系统要一大块（chunk = block_size * blocks_per_chunk），切成等长
// block 串成空闲链表；allocate/deallocate 退化成两次指针操作，没有系统调用。
class MemoryPool {
 public:
  MemoryPool(size_t block_size, size_t blocks_per_chunk = 512);
  ~MemoryPool();

  MemoryPool(const MemoryPool&) = delete;
  MemoryPool& operator=(const MemoryPool&) = delete;

  void* allocate();
  void deallocate(void* p);

  size_t blockSize() const { return block_size_; }
  size_t chunks() const { return chunks_.size(); }   // 向系统申请过几次
  size_t capacity() const { return capacity_; }       // 当前总 block 数
  size_t blocksInUse() const { return in_use_; }

 private:
  struct Chunk {
    char* base;
    size_t bytes;
  };
  void grow();

  size_t block_size_;
  size_t blocks_per_chunk_;
  std::vector<Chunk> chunks_;
  void* free_list_ = nullptr;
  size_t in_use_ = 0;
  size_t capacity_ = 0;
};

// 按 size class 取线程本地池：同一条线程上的分配完全不加锁。
// 池随线程退出而销毁。
MemoryPool& tlsPoolFor(size_t block_size);

// 把 STL 容器的单节点分配导到内存池上（n == 1 走池，数组走 operator new）。
template <typename T>
class PoolAllocator {
 public:
  using value_type = T;

  PoolAllocator() noexcept = default;
  template <typename U>
  PoolAllocator(const PoolAllocator<U>&) noexcept {}

  T* allocate(std::size_t n) {
    if (n == 1) return static_cast<T*>(tlsPoolFor(sizeof(T)).allocate());
    return static_cast<T*>(::operator new(n * sizeof(T)));
  }
  void deallocate(T* p, std::size_t n) noexcept {
    if (n == 1) {
      tlsPoolFor(sizeof(T)).deallocate(p);
    } else {
      ::operator delete(p);
    }
  }
  template <typename U>
  bool operator==(const PoolAllocator<U>&) const noexcept { return true; }
  template <typename U>
  bool operator!=(const PoolAllocator<U>&) const noexcept { return false; }
};

}  // namespace ws
