#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <queue>
#include <unordered_set>
#include <vector>

namespace ws {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using TimerId = uint64_t;
using TimerCallback = std::function<void()>;

inline TimePoint now() { return Clock::now(); }

// 最小堆定时器，用来回收空闲超时的连接。
//
// 取消采用「懒删除」：cancel() 只把 id 丢进 cancelled_ 集合，等这个定时器真的
// 从堆顶弹出来时再检查、跳过。理由是堆里按 key 删除要先找到节点再调整堆，
// 是 O(n)；而实际场景里「连接提前关闭」远比「定时器到期」少，懒删除把代价摊到了
// 本来就要执行的 pop 路径上，几乎免费。
class TimerHeap {
 public:
  TimerHeap() = default;

  TimerId add(TimePoint when, TimerCallback cb);
  TimerId addAfter(int64_t ms, TimerCallback cb);
  void cancel(TimerId id);

  // 弹出所有已到期的定时器并执行回调，返回到期个数。
  size_t expire();

  size_t size() const { return heap_.size(); }
  bool empty() const { return heap_.empty(); }

  // 距离下一个到期还有多少毫秒；没有定时器返回 -1。
  // 直接作为 epoll_wait 的 timeout 用，这样线程不会被空转唤醒，也不会睡过点。
  int nextTimeoutMs() const;

 private:
  struct Entry {
    TimePoint when;
    TimerCallback cb;
    TimerId id;
  };
  struct LaterFirst {
    bool operator()(const Entry& a, const Entry& b) const { return a.when > b.when; }
  };

  std::priority_queue<Entry, std::vector<Entry>, LaterFirst> heap_;
  std::unordered_set<TimerId> cancelled_;
  TimerId next_id_ = 1;
};

}  // namespace ws
